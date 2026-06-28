/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "lock_manager.h"

#include <algorithm>

bool LockManager::is_compatible(LockMode requested, LockMode granted) const {
    if (requested == LockMode::INTENTION_SHARED) {
        return granted != LockMode::EXLUCSIVE;
    }
    if (requested == LockMode::INTENTION_EXCLUSIVE) {
        return granted == LockMode::INTENTION_SHARED || granted == LockMode::INTENTION_EXCLUSIVE;
    }
    if (requested == LockMode::SHARED) {
        return granted == LockMode::INTENTION_SHARED || granted == LockMode::SHARED;
    }
    if (requested == LockMode::S_IX) {
        return granted == LockMode::INTENTION_SHARED;
    }
    return false;
}

bool LockManager::mode_covers(LockMode held, LockMode requested) const {
    if (held == requested || held == LockMode::EXLUCSIVE) {
        return true;
    }
    if (held == LockMode::S_IX &&
        (requested == LockMode::SHARED || requested == LockMode::INTENTION_EXCLUSIVE ||
         requested == LockMode::INTENTION_SHARED)) {
        return true;
    }
    if (held == LockMode::SHARED && requested == LockMode::INTENTION_SHARED) {
        return true;
    }
    if (held == LockMode::INTENTION_EXCLUSIVE && requested == LockMode::INTENTION_SHARED) {
        return true;
    }
    return false;
}

void LockManager::refresh_group_lock_mode(LockRequestQueue& queue) {
    bool has_is = false;
    bool has_ix = false;
    bool has_s = false;
    bool has_x = false;
    bool has_six = false;
    for (auto& request : queue.request_queue_) {
        if (!request.granted_) {
            continue;
        }
        has_is = has_is || request.lock_mode_ == LockMode::INTENTION_SHARED;
        has_ix = has_ix || request.lock_mode_ == LockMode::INTENTION_EXCLUSIVE;
        has_s = has_s || request.lock_mode_ == LockMode::SHARED;
        has_x = has_x || request.lock_mode_ == LockMode::EXLUCSIVE;
        has_six = has_six || request.lock_mode_ == LockMode::S_IX;
    }
    if (has_x) {
        queue.group_lock_mode_ = GroupLockMode::X;
    } else if (has_six || (has_s && has_ix)) {
        queue.group_lock_mode_ = GroupLockMode::SIX;
    } else if (has_s) {
        queue.group_lock_mode_ = GroupLockMode::S;
    } else if (has_ix) {
        queue.group_lock_mode_ = GroupLockMode::IX;
    } else if (has_is) {
        queue.group_lock_mode_ = GroupLockMode::IS;
    } else {
        queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
    }
}

bool LockManager::lock(Transaction* txn, const LockDataId& lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) {
        return true;
    }
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    if (txn->get_state() == TransactionState::ABORTED || txn->get_state() == TransactionState::COMMITTED) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    std::unique_lock<std::mutex> guard(latch_);
    auto& queue = lock_table_[lock_data_id];
    for (auto& request : queue.request_queue_) {
        if (request.txn_id_ == txn->get_transaction_id() && request.granted_) {
            if (mode_covers(request.lock_mode_, lock_mode)) {
                txn->get_lock_set()->insert(lock_data_id);
                return true;
            }

            for (auto& other : queue.request_queue_) {
                if (!other.granted_ || other.txn_id_ == txn->get_transaction_id()) {
                    continue;
                }
                if (!is_compatible(lock_mode, other.lock_mode_)) {
                    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
                }
            }
            request.lock_mode_ = lock_mode;
            refresh_group_lock_mode(queue);
            txn->get_lock_set()->insert(lock_data_id);
            return true;
        }
    }

    for (auto& request : queue.request_queue_) {
        if (!request.granted_ || request.txn_id_ == txn->get_transaction_id()) {
            continue;
        }
        if (!is_compatible(lock_mode, request.lock_mode_)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }

    LockRequest request(txn->get_transaction_id(), lock_mode);
    request.granted_ = true;
    queue.request_queue_.push_back(request);
    refresh_group_lock_mode(queue);
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    lock_IS_on_table(txn, tab_fd);
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    lock_IX_on_table(txn, tab_fd);
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) {
        return true;
    }
    std::unique_lock<std::mutex> guard(latch_);
    auto queue_it = lock_table_.find(lock_data_id);
    if (queue_it == lock_table_.end()) {
        return false;
    }

    auto& queue = queue_it->second;
    auto req_it = std::find_if(queue.request_queue_.begin(), queue.request_queue_.end(), [&](const LockRequest& request) {
        return request.txn_id_ == txn->get_transaction_id();
    });
    if (req_it == queue.request_queue_.end()) {
        return false;
    }

    queue.request_queue_.erase(req_it);
    refresh_group_lock_mode(queue);
    if (queue.request_queue_.empty()) {
        lock_table_.erase(queue_it);
    }
    if (txn->get_state() == TransactionState::GROWING) {
        txn->set_state(TransactionState::SHRINKING);
    }
    return true;
}
