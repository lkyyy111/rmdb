/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "log_recovery.h"

#include <cstring>
#include <memory>
#include <unistd.h>

#include "errors.h"
#include "record/rm_scan.h"

namespace {

std::string make_index_key_for_recovery(const std::vector<ColMeta>& cols, const char* record_data) {
    int total_len = 0;
    for (auto& col : cols) {
        total_len += col.len;
    }
    std::string key(total_len, '\0');
    int offset = 0;
    for (auto& col : cols) {
        memcpy(&key[offset], record_data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}

std::shared_ptr<LogRecord> make_log_record(LogType type) {
    switch (type) {
        case LogType::UPDATE:
            return std::make_shared<UpdateLogRecord>();
        case LogType::INSERT:
            return std::make_shared<InsertLogRecord>();
        case LogType::DELETE:
            return std::make_shared<DeleteLogRecord>();
        case LogType::begin:
            return std::make_shared<BeginLogRecord>();
        case LogType::commit:
            return std::make_shared<CommitLogRecord>();
        case LogType::ABORT:
            return std::make_shared<AbortLogRecord>();
    }
    return nullptr;
}

void insert_index_entries(SmManager* sm_manager, const std::string& tab_name, const RmRecord& record, const Rid& rid) {
    TabMeta& tab = sm_manager->db_.get_table(tab_name);
    for (auto& index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        std::string key = make_index_key_for_recovery(index.cols, record.data);
        std::vector<Rid> exists;
        if (!ih->get_value(key.data(), &exists, nullptr)) {
            ih->insert_entry(key.data(), rid, nullptr);
        }
    }
}

void delete_index_entries(SmManager* sm_manager, const std::string& tab_name, const RmRecord& record) {
    TabMeta& tab = sm_manager->db_.get_table(tab_name);
    for (auto& index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        std::string key = make_index_key_for_recovery(index.cols, record.data);
        ih->delete_entry(key.data(), nullptr);
    }
}

bool safe_is_record(RmFileHandle* fh, const Rid& rid) {
    try {
        return fh->is_record(rid);
    } catch (RMDBError&) {
        return false;
    }
}

void apply_insert(SmManager* sm_manager, const std::string& tab_name, const RmRecord& record, const Rid& rid) {
    RmFileHandle* fh = sm_manager->fhs_.at(tab_name).get();
    if (safe_is_record(fh, rid)) {
        auto current = fh->get_record(rid, nullptr);
        delete_index_entries(sm_manager, tab_name, *current);
        fh->update_record(rid, record.data, nullptr);
    } else {
        fh->insert_record(rid, record.data);
    }
    insert_index_entries(sm_manager, tab_name, record, rid);
}

void apply_delete(SmManager* sm_manager, const std::string& tab_name, const RmRecord& old_record, const Rid& rid) {
    RmFileHandle* fh = sm_manager->fhs_.at(tab_name).get();
    delete_index_entries(sm_manager, tab_name, old_record);
    if (safe_is_record(fh, rid)) {
        auto current = fh->get_record(rid, nullptr);
        delete_index_entries(sm_manager, tab_name, *current);
        fh->delete_record(rid, nullptr);
    }
}

void apply_update(SmManager* sm_manager, const std::string& tab_name, const RmRecord& new_record, const Rid& rid) {
    RmFileHandle* fh = sm_manager->fhs_.at(tab_name).get();
    if (safe_is_record(fh, rid)) {
        auto current = fh->get_record(rid, nullptr);
        delete_index_entries(sm_manager, tab_name, *current);
        fh->update_record(rid, new_record.data, nullptr);
    } else {
        fh->insert_record(rid, new_record.data);
    }
    insert_index_entries(sm_manager, tab_name, new_record, rid);
}

void flush_all_table_files(SmManager* sm_manager, BufferPoolManager* bpm) {
    for (auto& entry : sm_manager->fhs_) {
        entry.second->flush_file_hdr();
        bpm->flush_all_pages(entry.second->GetFd());
    }
}

void rebuild_all_indexes(SmManager* sm_manager) {
    for (auto& fh_entry : sm_manager->fhs_) {
        const std::string& tab_name = fh_entry.first;
        TabMeta& tab = sm_manager->db_.get_table(tab_name);
        if (tab.indexes.empty()) {
            continue;
        }
        RmFileHandle* fh = fh_entry.second.get();

        for (auto& index : tab.indexes) {
            std::string ix_name = sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols);
            sm_manager->ihs_.at(ix_name)->clear_entries();
        }

        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            auto record = fh->get_record(scan.rid(), nullptr);
            for (auto& index : tab.indexes) {
                std::string ix_name = sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols);
                std::string key = make_index_key_for_recovery(index.cols, record->data);
                sm_manager->ihs_.at(ix_name)->insert_entry(key.data(), scan.rid(), nullptr);
            }
        }
    }
}

}  // namespace

void RecoveryManager::analyze() {
    logs_.clear();
    committed_txns_.clear();
    finished_txns_.clear();

    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) {
        return;
    }

    std::vector<char> data(file_size);
    int read_size = disk_manager_->read_log(data.data(), file_size, 0);
    if (read_size <= 0) {
        return;
    }

    int offset = 0;
    while (offset + LOG_HEADER_SIZE <= read_size) {
        LogType type = *reinterpret_cast<const LogType*>(data.data() + offset + OFFSET_LOG_TYPE);
        uint32_t total_len = *reinterpret_cast<const uint32_t*>(data.data() + offset + OFFSET_LOG_TOT_LEN);
        if (total_len < LOG_HEADER_SIZE || offset + static_cast<int>(total_len) > read_size) {
            break;
        }

        auto log_record = make_log_record(type);
        if (log_record == nullptr) {
            break;
        }
        log_record->deserialize(data.data() + offset);
        if (log_record->log_type_ == LogType::commit) {
            committed_txns_.insert(log_record->log_tid_);
            finished_txns_.insert(log_record->log_tid_);
        } else if (log_record->log_type_ == LogType::ABORT) {
            finished_txns_.insert(log_record->log_tid_);
        }
        logs_.push_back(log_record);
        offset += static_cast<int>(total_len);
    }
}

void RecoveryManager::redo() {
    for (auto& log_record : logs_) {
        if (committed_txns_.find(log_record->log_tid_) == committed_txns_.end()) {
            continue;
        }
        if (log_record->log_type_ == LogType::INSERT) {
            auto log = std::static_pointer_cast<InsertLogRecord>(log_record);
            apply_insert(sm_manager_, log->table_name(), log->insert_value_, log->rid_);
        } else if (log_record->log_type_ == LogType::DELETE) {
            auto log = std::static_pointer_cast<DeleteLogRecord>(log_record);
            apply_delete(sm_manager_, log->table_name(), log->delete_value_, log->rid_);
        } else if (log_record->log_type_ == LogType::UPDATE) {
            auto log = std::static_pointer_cast<UpdateLogRecord>(log_record);
            apply_update(sm_manager_, log->table_name(), log->new_value_, log->rid_);
        }
    }
}

void RecoveryManager::undo() {
    for (auto it = logs_.rbegin(); it != logs_.rend(); ++it) {
        auto& log_record = *it;
        if (finished_txns_.find(log_record->log_tid_) != finished_txns_.end()) {
            continue;
        }
        if (log_record->log_type_ == LogType::INSERT) {
            auto log = std::static_pointer_cast<InsertLogRecord>(log_record);
            apply_delete(sm_manager_, log->table_name(), log->insert_value_, log->rid_);
        } else if (log_record->log_type_ == LogType::DELETE) {
            auto log = std::static_pointer_cast<DeleteLogRecord>(log_record);
            apply_insert(sm_manager_, log->table_name(), log->delete_value_, log->rid_);
        } else if (log_record->log_type_ == LogType::UPDATE) {
            auto log = std::static_pointer_cast<UpdateLogRecord>(log_record);
            apply_update(sm_manager_, log->table_name(), log->old_value_, log->rid_);
        }
    }

    flush_all_table_files(sm_manager_, buffer_pool_manager_);
    rebuild_all_indexes(sm_manager_);
    int log_fd = disk_manager_->GetLogFd();
    if (log_fd != -1) {
        ftruncate(log_fd, 0);
        lseek(log_fd, 0, SEEK_SET);
    } else {
        truncate(LOG_FILE_NAME.c_str(), 0);
    }
}
