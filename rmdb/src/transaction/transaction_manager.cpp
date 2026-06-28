/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "transaction_manager.h"

#include <algorithm>
#include <cstring>

#include "errors.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

namespace {

std::string make_index_key_for_txn(const std::vector<ColMeta>& cols, const char* record_data) {
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

void append_log(Transaction* txn, LogManager* log_manager, LogRecord* log_record) {
    if (txn == nullptr || log_manager == nullptr || log_record == nullptr) {
        return;
    }
    log_record->prev_lsn_ = txn->get_prev_lsn();
    lsn_t lsn = log_manager->add_log_to_buffer(log_record);
    txn->set_prev_lsn(lsn);
    log_manager->flush_log_to_disk();
}

void clear_write_set(Transaction* txn) {
    if (txn == nullptr || txn->get_write_set() == nullptr) {
        return;
    }
    for (auto* record : *txn->get_write_set()) {
        delete record;
    }
    txn->get_write_set()->clear();
}

void insert_index_entries(SmManager* sm_manager, const std::string& tab_name, const RmRecord& record, const Rid& rid) {
    TabMeta& tab = sm_manager->db_.get_table(tab_name);
    for (auto& index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        std::string key = make_index_key_for_txn(index.cols, record.data);
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
        std::string key = make_index_key_for_txn(index.cols, record.data);
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

}  // namespace

Transaction *TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }
    txn->set_state(TransactionState::GROWING);
    txn->set_start_ts(next_timestamp_++);
    txn->set_prev_lsn(INVALID_LSN);

    BeginLogRecord log_record(txn->get_transaction_id());
    append_log(txn, log_manager, &log_record);

    std::unique_lock<std::mutex> lock(latch_);
    TransactionManager::txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        return;
    }
    if (txn->get_state() == TransactionState::ABORTED || txn->get_state() == TransactionState::COMMITTED) {
        return;
    }

    CommitLogRecord log_record(txn->get_transaction_id());
    append_log(txn, log_manager, &log_record);

    if (txn->get_lock_set() != nullptr) {
        std::vector<LockDataId> locks(txn->get_lock_set()->begin(), txn->get_lock_set()->end());
        for (auto& lock_data_id : locks) {
            lock_manager_->unlock(txn, lock_data_id);
        }
        txn->get_lock_set()->clear();
    }
    clear_write_set(txn);
    if (txn->get_index_latch_page_set() != nullptr) {
        txn->get_index_latch_page_set()->clear();
    }
    if (txn->get_index_deleted_page_set() != nullptr) {
        txn->get_index_deleted_page_set()->clear();
    }
    txn->set_state(TransactionState::COMMITTED);
}

void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    if (txn->get_state() == TransactionState::ABORTED) {
        return;
    }

    if (txn->get_write_set() != nullptr) {
        auto& writes = *txn->get_write_set();
        for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
            WriteRecord* write = *it;
            const std::string tab_name = write->GetTableName();
            RmFileHandle* fh = sm_manager_->fhs_.at(tab_name).get();
            Rid rid = write->GetRid();
            if (write->GetWriteType() == WType::INSERT_TUPLE) {
                if (safe_is_record(fh, rid)) {
                    auto record = fh->get_record(rid, nullptr);
                    delete_index_entries(sm_manager_, tab_name, *record);
                    fh->delete_record(rid, nullptr);
                }
            } else if (write->GetWriteType() == WType::DELETE_TUPLE) {
                RmRecord& old_record = write->GetRecord();
                if (safe_is_record(fh, rid)) {
                    auto current = fh->get_record(rid, nullptr);
                    delete_index_entries(sm_manager_, tab_name, *current);
                    fh->update_record(rid, old_record.data, nullptr);
                } else {
                    fh->insert_record(rid, old_record.data);
                }
                insert_index_entries(sm_manager_, tab_name, old_record, rid);
            } else if (write->GetWriteType() == WType::UPDATE_TUPLE) {
                RmRecord& old_record = write->GetRecord();
                if (safe_is_record(fh, rid)) {
                    auto current = fh->get_record(rid, nullptr);
                    delete_index_entries(sm_manager_, tab_name, *current);
                    fh->update_record(rid, old_record.data, nullptr);
                } else {
                    fh->insert_record(rid, old_record.data);
                }
                insert_index_entries(sm_manager_, tab_name, old_record, rid);
            }
        }
    }

    AbortLogRecord log_record(txn->get_transaction_id());
    append_log(txn, log_manager, &log_record);

    if (txn->get_lock_set() != nullptr) {
        std::vector<LockDataId> locks(txn->get_lock_set()->begin(), txn->get_lock_set()->end());
        for (auto& lock_data_id : locks) {
            lock_manager_->unlock(txn, lock_data_id);
        }
        txn->get_lock_set()->clear();
    }
    clear_write_set(txn);
    if (txn->get_index_latch_page_set() != nullptr) {
        txn->get_index_latch_page_set()->clear();
    }
    if (txn->get_index_deleted_page_set() != nullptr) {
        txn->get_index_deleted_page_set()->clear();
    }
    txn->set_state(TransactionState::ABORTED);
}
