// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/runtime/runtime_state.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "runtime/runtime_state.h"

#include <boost/algorithm/string/join.hpp>
#include <memory>
#include <sstream>
#include <string>

#include "common/logging.h"
#include "common/object_pool.h"
#include "common/status.h"
#include "exec/exec_node.h"
#include "exprs/vectorized/runtime_filter_bank.h"
#include "runtime/buffered_block_mgr2.h"
#include "runtime/bufferpool/reservation_tracker.h"
#include "runtime/bufferpool/reservation_util.h"
#include "runtime/descriptors.h"
#include "runtime/exec_env.h"
#include "runtime/initial_reservations.h"
#include "runtime/load_path_mgr.h"
#include "runtime/mem_tracker.h"
#include "runtime/runtime_filter_worker.h"
#include "util/load_error_hub.h"
#include "util/pretty_printer.h"
#include "util/timezone_utils.h"
#include "util/uid_util.h"

namespace starrocks {

// for ut only
RuntimeState::RuntimeState(const TUniqueId& fragment_instance_id, const TQueryOptions& query_options,
                           const TQueryGlobals& query_globals, ExecEnv* exec_env)
        : _profile("Fragment " + print_id(fragment_instance_id)),
          _unreported_error_idx(0),
          _fragment_mem_tracker(NULL),
          _obj_pool(new ObjectPool()),
          _is_cancelled(false),
          _per_fragment_instance_idx(0),
          _root_node_id(-1),
          _num_rows_load_total(0),
          _num_rows_load_filtered(0),
          _num_rows_load_unselected(0),
          _num_print_error_rows(0),
          _error_log_file_path(),
          _error_log_file(nullptr),
          _instance_buffer_reservation(new ReservationTracker) {
    Status status = init(fragment_instance_id, query_options, query_globals, exec_env);
    DCHECK(status.ok());
}

RuntimeState::RuntimeState(const TExecPlanFragmentParams& fragment_params, const TQueryOptions& query_options,
                           const TQueryGlobals& query_globals, ExecEnv* exec_env)
        : _profile("Fragment " + print_id(fragment_params.params.fragment_instance_id)),
          _unreported_error_idx(0),
          _query_id(fragment_params.params.query_id),
          _fragment_mem_tracker(NULL),
          _obj_pool(new ObjectPool()),
          _is_cancelled(false),
          _per_fragment_instance_idx(0),
          _root_node_id(-1),
          _num_rows_load_total(0),
          _num_rows_load_filtered(0),
          _num_rows_load_unselected(0),
          _num_print_error_rows(0),
          _error_log_file_path(),
          _error_log_file(nullptr),
          _instance_buffer_reservation(new ReservationTracker) {
    Status status = init(fragment_params.params.fragment_instance_id, query_options, query_globals, exec_env);
    DCHECK(status.ok());
}

RuntimeState::RuntimeState(const TQueryGlobals& query_globals)
        : _profile("<unnamed>"),
          _unreported_error_idx(0),
          _obj_pool(new ObjectPool()),
          _is_cancelled(false),
          _per_fragment_instance_idx(0) {
    _query_options.batch_size = DEFAULT_BATCH_SIZE;
    if (query_globals.__isset.time_zone) {
        _timezone = query_globals.time_zone;
        _timestamp_ms = query_globals.timestamp_ms;
    } else if (!query_globals.now_string.empty()) {
        _timezone = TimezoneUtils::default_time_zone;
        DateTimeValue dt;
        dt.from_date_str(query_globals.now_string.c_str(), query_globals.now_string.size());
        int64_t timestamp;
        dt.unix_timestamp(&timestamp, _timezone);
        _timestamp_ms = timestamp * 1000;
    } else {
        //Unit test may set into here
        _timezone = TimezoneUtils::default_time_zone;
        _timestamp_ms = 0;
    }
    TimezoneUtils::find_cctz_time_zone(_timezone, _timezone_obj);
}

RuntimeState::~RuntimeState() {
    _block_mgr2.reset();
    // close error log file
    if (_error_log_file != nullptr && _error_log_file->is_open()) {
        _error_log_file->close();
        delete _error_log_file;
        _error_log_file = nullptr;
    }

    if (_error_hub != nullptr) {
        _error_hub->close();
    }

    // Release the reservation, which should be unused at the point.
    if (_instance_buffer_reservation != nullptr) {
        _instance_buffer_reservation->Close();
    }

    if (_initial_reservations != nullptr) {
        _initial_reservations->ReleaseResources();
    }

    if (_buffer_reservation != nullptr) {
        _buffer_reservation->Close();
    }

    if (_exec_env != nullptr && _exec_env->thread_mgr() != nullptr) {
        _exec_env->thread_mgr()->unregister_pool(_resource_pool);
    }

#ifndef BE_TEST
    // LogUsage() walks the MemTracker tree top-down when the memory limit is exceeded.
    // Break the link between the instance_mem_tracker and its parent (_query_mem_tracker)
    // before the _instance_mem_tracker and its children are destroyed.
    if (_instance_mem_tracker.get() != NULL) {
        // May be NULL if InitMemTrackers() is not called, for example from tests.
        _instance_mem_tracker->close();
    }

    if (_query_mem_tracker.get() != NULL) {
        _query_mem_tracker->close();
    }
#endif
}

Status RuntimeState::init(const TUniqueId& fragment_instance_id, const TQueryOptions& query_options,
                          const TQueryGlobals& query_globals, ExecEnv* exec_env) {
    _fragment_instance_id = fragment_instance_id;
    _query_options = query_options;
    if (query_globals.__isset.time_zone) {
        _timezone = query_globals.time_zone;
        _timestamp_ms = query_globals.timestamp_ms;
    } else if (!query_globals.now_string.empty()) {
        _timezone = TimezoneUtils::default_time_zone;
        DateTimeValue dt;
        dt.from_date_str(query_globals.now_string.c_str(), query_globals.now_string.size());
        int64_t timestamp;
        dt.unix_timestamp(&timestamp, _timezone);
        _timestamp_ms = timestamp * 1000;
    } else {
        //Unit test may set into here
        _timezone = TimezoneUtils::default_time_zone;
        _timestamp_ms = 0;
    }
    if (query_globals.__isset.last_query_id) {
        _last_query_id = query_globals.last_query_id;
    }
    TimezoneUtils::find_cctz_time_zone(_timezone, _timezone_obj);

    _exec_env = exec_env;

    if (_query_options.max_errors <= 0) {
        // TODO: fix linker error and uncomment this
        //_query_options.max_errors = config::max_errors;
        _query_options.max_errors = 100;
    }

    if (_query_options.batch_size <= 0) {
        _query_options.batch_size = DEFAULT_BATCH_SIZE;
    }

    // Register with the thread mgr
    if (exec_env != NULL) {
        _resource_pool = exec_env->thread_mgr()->register_pool();
        DCHECK(_resource_pool != NULL);
    }
    _db_name = "insert_stmt";
    _import_label = print_id(fragment_instance_id);
    _runtime_filter_port = _obj_pool->add(new RuntimeFilterPort(this));

    return Status::OK();
}

Status RuntimeState::init_mem_trackers(const TUniqueId& query_id) {
    bool has_query_mem_tracker = _query_options.__isset.mem_limit && (_query_options.mem_limit > 0);
    int64_t bytes_limit = has_query_mem_tracker ? _query_options.mem_limit : -1;
    auto* mem_tracker_counter = ADD_COUNTER(&_profile, "MemoryLimit", TUnit::BYTES);
    mem_tracker_counter->set(bytes_limit);

    _query_mem_tracker = std::make_unique<MemTracker>(MemTracker::QUERY, bytes_limit, runtime_profile()->name(),
                                                      _exec_env->query_pool_mem_tracker());
    _instance_mem_tracker =
            std::make_unique<MemTracker>(&_profile, -1, runtime_profile()->name(), _query_mem_tracker.get());
    RETURN_IF_ERROR(init_buffer_poolstate());

    _initial_reservations =
            _obj_pool->add(new InitialReservations(_obj_pool.get(), _buffer_reservation, _query_mem_tracker.get(),
                                                   _query_options.initial_reservation_total_claims));
    RETURN_IF_ERROR(_initial_reservations->Init(_query_id, min_reservation()));
    DCHECK_EQ(0, _initial_reservation_refcnt.load());

    if (_instance_buffer_reservation != nullptr) {
        _instance_buffer_reservation->InitChildTracker(&_profile, _buffer_reservation, _instance_mem_tracker.get(),
                                                       std::numeric_limits<int64_t>::max());
    }

    return Status::OK();
}

Status RuntimeState::init_instance_mem_tracker() {
    _instance_mem_tracker = std::make_unique<MemTracker>(-1);
    return Status::OK();
}

Status RuntimeState::init_buffer_poolstate() {
    ExecEnv* exec_env = ExecEnv::GetInstance();
    int64_t mem_limit = _query_mem_tracker->lowest_limit();
    int64_t max_reservation;
    if (query_options().__isset.buffer_pool_limit && query_options().buffer_pool_limit > 0) {
        max_reservation = query_options().buffer_pool_limit;
    } else if (mem_limit == -1) {
        // No query mem limit. The process-wide reservation limit is the only limit on
        // reservations.
        max_reservation = std::numeric_limits<int64_t>::max();
    } else {
        DCHECK_GE(mem_limit, 0);
        max_reservation = ReservationUtil::GetReservationLimitFromMemLimit(mem_limit);
    }
    _buffer_reservation = _obj_pool->add(new ReservationTracker);
    _buffer_reservation->InitChildTracker(NULL, exec_env->buffer_reservation(), _query_mem_tracker.get(),
                                          max_reservation);
    return Status::OK();
}

Status RuntimeState::create_block_mgr() {
    DCHECK(_block_mgr2.get() == NULL);

    int64_t block_mgr_limit = _query_mem_tracker->limit();
    if (block_mgr_limit < 0) {
        block_mgr_limit = std::numeric_limits<int64_t>::max();
    }
    RETURN_IF_ERROR(BufferedBlockMgr2::create(this, _query_mem_tracker.get(), runtime_profile(),
                                              _exec_env->tmp_file_mgr(), block_mgr_limit,
                                              _exec_env->disk_io_mgr()->max_read_buffer_size(), &_block_mgr2));
    return Status::OK();
}

std::string RuntimeState::error_log() {
    std::lock_guard<std::mutex> l(_error_log_lock);
    return boost::algorithm::join(_error_log, "\n");
}

bool RuntimeState::log_error(const std::string& error) {
    std::lock_guard<std::mutex> l(_error_log_lock);

    if (_error_log.size() < _query_options.max_errors) {
        _error_log.push_back(error);
        return true;
    }

    return false;
}

void RuntimeState::log_error(const Status& status) {
    if (status.ok()) {
        return;
    }

    log_error(status.get_error_msg());
}

void RuntimeState::get_unreported_errors(std::vector<std::string>* new_errors) {
    std::lock_guard<std::mutex> l(_error_log_lock);

    if (_unreported_error_idx < _error_log.size()) {
        new_errors->assign(_error_log.begin() + _unreported_error_idx, _error_log.end());
        _unreported_error_idx = _error_log.size();
    }
}

Status RuntimeState::set_mem_limit_exceeded(MemTracker* tracker, int64_t failed_allocation_size,
                                            const std::string* msg) {
    DCHECK_GE(failed_allocation_size, 0);
    {
        std::lock_guard<std::mutex> l(_process_status_lock);
        if (_process_status.ok()) {
            if (msg != nullptr) {
                _process_status = Status::MemoryLimitExceeded(*msg);
            } else {
                _process_status = Status::MemoryLimitExceeded("Memory limit exceeded");
            }
        } else {
            return _process_status;
        }
    }

    DCHECK(_query_mem_tracker.get() != NULL);
    std::stringstream ss;
    ss << "Memory Limit Exceeded\n";
    if (failed_allocation_size != 0) {
        DCHECK(tracker != NULL);
        ss << "  " << tracker->label() << " could not allocate "
           << PrettyPrinter::print(failed_allocation_size, TUnit::BYTES) << " without exceeding limit." << std::endl;
    }

    log_error(ss.str());
    DCHECK(_process_status.is_mem_limit_exceeded());
    return _process_status;
}

Status RuntimeState::check_query_state(const std::string& msg) {
    // TODO: it would be nice if this also checked for cancellation, but doing so breaks
    // cases where we use Status::Cancelled("Cancelled") to indicate that the limit was reached.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull-compare"
    RETURN_IF_LIMIT_EXCEEDED(this, msg);
#pragma pop
    return query_status();
}

const int64_t MAX_ERROR_NUM = 50;

Status RuntimeState::create_error_log_file() {
    _exec_env->load_path_mgr()->get_load_error_file_name(_db_name, _import_label, _fragment_instance_id,
                                                         &_error_log_file_path);
    std::string error_log_absolute_path =
            _exec_env->load_path_mgr()->get_load_error_absolute_path(_error_log_file_path);
    _error_log_file = new std::ofstream(error_log_absolute_path, std::ifstream::out);
    if (!_error_log_file->is_open()) {
        std::stringstream error_msg;
        error_msg << "Fail to open error file: [" << _error_log_file_path << "].";
        LOG(WARNING) << error_msg.str();
        return Status::InternalError(error_msg.str());
    }
    return Status::OK();
}

void RuntimeState::append_error_msg_to_file(const std::string& line, const std::string& error_msg, bool is_summary) {
    if (_query_options.query_type != TQueryType::LOAD) {
        return;
    }
    // If file havn't been opened, open it here
    if (_error_log_file == nullptr) {
        Status status = create_error_log_file();
        if (!status.ok()) {
            LOG(WARNING) << "Create error file log failed. because: " << status.get_error_msg();
            if (_error_log_file != nullptr) {
                _error_log_file->close();
                delete _error_log_file;
                _error_log_file = nullptr;
            }
            return;
        }
    }

    // if num of printed error row exceeds the limit, and this is not a summary message,
    // return
    if (_num_print_error_rows.fetch_add(1, std::memory_order_relaxed) > MAX_ERROR_NUM && !is_summary) {
        return;
    }

    std::stringstream out;
    if (is_summary) {
        out << "Summary: ";
        out << error_msg;
    } else {
        // Note: export reason first in case src line too long and be truncated.
        out << "Reason: " << error_msg;
        out << ". src line: [" << line << "]; ";
    }

    if (!out.str().empty()) {
        (*_error_log_file) << out.str() << std::endl;
        export_load_error(out.str());
    }
}

const int64_t HUB_MAX_ERROR_NUM = 10;

void RuntimeState::export_load_error(const std::string& err_msg) {
    if (_error_hub == nullptr) {
        if (_load_error_hub_info == nullptr) {
            return;
        }
        LoadErrorHub::create_hub(_exec_env, _load_error_hub_info.get(), _error_log_file_path, &_error_hub);
    }

    LoadErrorHub::ErrorMsg err(_load_job_id, err_msg);
    // TODO(lingbin): think if should check return value?
    _error_hub->export_error(err);
}

int64_t RuntimeState::get_load_mem_limit() const {
    if (_query_options.__isset.load_mem_limit && _query_options.load_mem_limit > 0) {
        return _query_options.load_mem_limit;
    }
    return 0;
}

} // end namespace starrocks
