// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/journal/local/LocalJournalCursor.java

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

package com.starrocks.journal.local;

import com.starrocks.alter.AlterJob;
import com.starrocks.catalog.Database;
import com.starrocks.common.io.Text;
import com.starrocks.ha.MasterInfo;
import com.starrocks.journal.JournalCursor;
import com.starrocks.journal.JournalEntity;
import com.starrocks.journal.bdbje.Timestamp;
import com.starrocks.load.DeleteInfo;
import com.starrocks.load.LoadErrorHub;
import com.starrocks.load.MultiDeleteInfo;
import com.starrocks.persist.BatchDropInfo;
import com.starrocks.persist.BatchModifyPartitionsInfo;
import com.starrocks.persist.ConsistencyCheckInfo;
import com.starrocks.persist.CreateTableInfo;
import com.starrocks.persist.DatabaseInfo;
import com.starrocks.persist.DropInfo;
import com.starrocks.persist.DropPartitionInfo;
import com.starrocks.persist.EditLogFileInputStream;
import com.starrocks.persist.ModifyPartitionInfo;
import com.starrocks.persist.OperationType;
import com.starrocks.persist.PartitionPersistInfo;
import com.starrocks.persist.RecoverInfo;
import com.starrocks.persist.ReplicaPersistInfo;
import com.starrocks.persist.Storage;
import com.starrocks.persist.TableInfo;
import com.starrocks.system.Backend;
import com.starrocks.system.Frontend;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.util.List;

@Deprecated
public final class LocalJournalCursor implements JournalCursor {
    private static final Logger LOG = LogManager.getLogger(LocalJournalCursor.class);
    private String imageDir;
    private long toKey;
    private long currentKey;
    private List<Long> editFileSequenceNumbers;
    private int nextFilePositionIndex;
    private Storage storage;
    private DataInputStream currentStream;

    public static LocalJournalCursor getJournalCursor(String imageDir, long fromKey, long toKey) {
        if (toKey < fromKey && toKey != -1 || fromKey < 0) {
            System.out.println("Invalid key range!");
            return null;
        }
        long newToKey = toKey;
        if (newToKey == -1) {
            newToKey = Long.MAX_VALUE;
        }
        LocalJournalCursor cursor;
        try {
            cursor = new LocalJournalCursor(imageDir, fromKey, newToKey);
        } catch (IOException e) {
            LOG.error(e);
            return null;
        }
        return cursor;
    }

    /*
     * edits file name is the minimum journal id in this file.
     * For example:
     * an edit file contains journal from 100 to 200. its file name is edits.100
     */
    private LocalJournalCursor(String imageDir, long fromKey, long toKey) throws IOException {
        this.imageDir = imageDir;
        this.currentKey = fromKey;
        this.toKey = toKey;
        this.storage = new Storage(imageDir);
        this.editFileSequenceNumbers = storage.getEditsFileSequenceNumbers();
        this.nextFilePositionIndex = 0;
        long scannedKey = 0;

        // find the file which may contain the journal with journalId=fromKey
        String fileName = null;
        for (long minJournalKey : editFileSequenceNumbers) {
            if (fromKey >= minJournalKey) {
                fileName = Long.toString(minJournalKey);
                nextFilePositionIndex++;
                scannedKey = minJournalKey;
                continue;
            } else {
                break;
            }
        }

        if (fileName == null) {
            System.out.println("Can not find the key:" + fromKey);
            throw new IOException();
        }

        this.currentStream = new DataInputStream(new BufferedInputStream(
                new EditLogFileInputStream(new File(imageDir, "edits." + fileName))));

        while (scannedKey < fromKey) {
            short opCode = currentStream.readShort();
            if (opCode == OperationType.OP_INVALID) {
                System.out.println("Can not find the key:" + fromKey);
                throw new IOException();
            }
            getJournalEntity(currentStream, opCode);
            scannedKey++;
        }
    }

    @Override
    public JournalEntity next() {
        if (currentKey > toKey) {
            return null;
        }

        JournalEntity ret = null;
        try {
            short opCode = OperationType.OP_INVALID;

            while (true) {
                try {
                    opCode = currentStream.readShort();
                    if (opCode == OperationType.OP_INVALID) {
                        if (nextFilePositionIndex < editFileSequenceNumbers.size()) {
                            currentStream.close();
                            currentStream = new DataInputStream(new BufferedInputStream(new EditLogFileInputStream(
                                    new File(imageDir, "edits." + editFileSequenceNumbers
                                            .get(nextFilePositionIndex)))));
                            nextFilePositionIndex++;
                            continue;
                        } else {
                            return null;
                        }
                    }
                } catch (EOFException e) {
                    if (nextFilePositionIndex < editFileSequenceNumbers.size()) {
                        currentStream.close();
                        currentStream = new DataInputStream(
                                new BufferedInputStream(new EditLogFileInputStream(new File(
                                        imageDir, "edits." + editFileSequenceNumbers.get(nextFilePositionIndex)))));
                        nextFilePositionIndex++;
                        continue;
                    } else {
                        return null;
                    }
                }
                break;
            }

            ret = getJournalEntity(currentStream, opCode);
            currentKey++;
            return ret;
        } catch (IOException e) {
            LOG.error("something wrong. {}", e);
            try {
                currentStream.close();
            } catch (IOException e1) {
                LOG.error(e1);
            }
            LOG.error(e);
        }
        return ret;
    }

    @Deprecated
    private JournalEntity getJournalEntity(DataInputStream in, short opCode) throws IOException {
        JournalEntity ret = new JournalEntity();
        ret.setOpCode(opCode);
        switch (opCode) {
            case OperationType.OP_SAVE_NEXTID: {
                Text text = new Text();
                text.readFields(in);
                ret.setData(text);
                break;
            }
            case OperationType.OP_SAVE_TRANSACTION_ID: {
                Text text = new Text();
                text.readFields(in);
                ret.setData(text);
                break;
            }
            case OperationType.OP_CREATE_DB: {
                Database db = new Database();
                db.readFields(in);
                ret.setData(db);
                break;
            }
            case OperationType.OP_DROP_DB: {
                Text text = new Text();
                text.readFields(in);
                ret.setData(text);
                break;
            }
            case OperationType.OP_ALTER_DB:
            case OperationType.OP_RENAME_DB: {
                DatabaseInfo info = new DatabaseInfo();
                info.readFields(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_CREATE_TABLE: {
                CreateTableInfo info = new CreateTableInfo();
                info.readFields(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_DROP_TABLE: {
                DropInfo info = new DropInfo();
                info.readFields(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_ADD_PARTITION: {
                PartitionPersistInfo info = new PartitionPersistInfo();
                info.readFields(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_DROP_PARTITION: {
                DropPartitionInfo info = DropPartitionInfo.read(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_MODIFY_PARTITION: {
                ModifyPartitionInfo info = ModifyPartitionInfo.read(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_BATCH_MODIFY_PARTITION: {
                BatchModifyPartitionsInfo info = BatchModifyPartitionsInfo.read(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_ERASE_DB:
            case OperationType.OP_ERASE_TABLE:
            case OperationType.OP_ERASE_PARTITION: {
                Text text = new Text();
                text.readFields(in);
                ret.setData(text);
                break;
            }
            case OperationType.OP_RECOVER_DB:
            case OperationType.OP_RECOVER_TABLE:
            case OperationType.OP_RECOVER_PARTITION: {
                RecoverInfo recoverInfo = new RecoverInfo();
                recoverInfo.readFields(in);
                ret.setData(recoverInfo);
                break;
            }
            case OperationType.OP_START_ROLLUP:
            case OperationType.OP_FINISH_ROLLUP:
            case OperationType.OP_CANCEL_ROLLUP:
            case OperationType.OP_START_SCHEMA_CHANGE:
            case OperationType.OP_FINISH_SCHEMA_CHANGE:
            case OperationType.OP_CANCEL_SCHEMA_CHANGE:
            case OperationType.OP_START_DECOMMISSION_BACKEND:
            case OperationType.OP_FINISH_DECOMMISSION_BACKEND: {
                AlterJob alterJob = AlterJob.read(in);
                ret.setData(alterJob);
                break;
            }
            case OperationType.OP_CLEAR_ROLLUP_INFO: {
                ReplicaPersistInfo info = ReplicaPersistInfo.read(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_DROP_ROLLUP: {
                DropInfo info = DropInfo.read(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_BATCH_DROP_ROLLUP: {
                BatchDropInfo batchDropInfo = BatchDropInfo.read(in);
                ret.setData(batchDropInfo);
                break;
            }
            case OperationType.OP_RENAME_TABLE:
            case OperationType.OP_RENAME_ROLLUP:
            case OperationType.OP_RENAME_PARTITION: {
                TableInfo info = TableInfo.read(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_FINISH_CONSISTENCY_CHECK: {
                ConsistencyCheckInfo info = ConsistencyCheckInfo.read(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_FINISH_DELETE: {
                DeleteInfo info = new DeleteInfo();
                info.readFields(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_FINISH_MULTI_DELETE: {
                MultiDeleteInfo info = MultiDeleteInfo.read(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_ADD_REPLICA:
            case OperationType.OP_DELETE_REPLICA: {
                ReplicaPersistInfo info = ReplicaPersistInfo.read(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_ADD_BACKEND:
            case OperationType.OP_DROP_BACKEND:
            case OperationType.OP_BACKEND_STATE_CHANGE: {
                Backend be = Backend.read(in);
                ret.setData(be);
                break;
            }
            case OperationType.OP_ADD_FRONTEND:
            case OperationType.OP_ADD_FIRST_FRONTEND:
            case OperationType.OP_REMOVE_FRONTEND: {
                Frontend fe = Frontend.read(in);
                ret.setData(fe);
                break;
            }
            case OperationType.OP_SET_LOAD_ERROR_HUB: {
                LoadErrorHub.Param param = new LoadErrorHub.Param();
                param.readFields(in);
                ret.setData(param);
                break;
            }
            case OperationType.OP_MASTER_INFO_CHANGE: {
                MasterInfo info = new MasterInfo();
                info.readFields(in);
                ret.setData(info);
                break;
            }
            case OperationType.OP_TIMESTAMP: {
                Timestamp stamp = new Timestamp();
                stamp.readFields(in);
                ret.setData(stamp);
                break;
            }
            case OperationType.OP_META_VERSION: {
                Text text = new Text();
                text.readFields(in);
                ret.setData(text);
                break;
            }

            default: {
                throw new IOException("Never seen opcode " + opCode);
            }
        }
        return ret;
    }

    @Override
    public void close() {

    }
}

