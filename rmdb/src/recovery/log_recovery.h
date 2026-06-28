/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <memory>
#include <unordered_set>
#include <vector>

#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"

class RedoLogsInPage {
   public:
    RedoLogsInPage() { table_file_ = nullptr; }
    RmFileHandle* table_file_;
    std::vector<lsn_t> redo_logs_;
};

class RecoveryManager {
   public:
    RecoveryManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager, SmManager* sm_manager) {
        disk_manager_ = disk_manager;
        buffer_pool_manager_ = buffer_pool_manager;
        sm_manager_ = sm_manager;
    }

    void analyze();
    void redo();
    void undo();

   private:
    std::vector<std::shared_ptr<LogRecord>> logs_;
    std::unordered_set<txn_id_t> committed_txns_;
    std::unordered_set<txn_id_t> finished_txns_;
    LogBuffer buffer_;
    DiskManager* disk_manager_;
    BufferPoolManager* buffer_pool_manager_;
    SmManager* sm_manager_;
};
