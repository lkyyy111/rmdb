/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include <set>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());

        struct UpdateItem {
            Rid rid;
            std::unique_ptr<RmRecord> old_record;
            std::unique_ptr<RmRecord> new_record;
        };
        std::vector<UpdateItem> items;
        for (auto &rid : rids_) {
            auto record = fh_->get_record(rid, context_);
            auto new_record = std::make_unique<RmRecord>(*record);
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                if (!coerce_value_to_col_type(set_clause.rhs, col->type)) {
                    throw IncompatibleTypeError(coltype2str(col->type), coltype2str(set_clause.rhs.type));
                }
                if (set_clause.rhs.raw == nullptr) {
                    set_clause.rhs.init_raw(col->len);
                }
                memcpy(new_record->data + col->offset, set_clause.rhs.raw->data, col->len);
            }
            items.push_back(UpdateItem{rid, std::move(record), std::move(new_record)});
        }

        for (auto &index : tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            std::set<std::string> new_keys;
            for (auto &item : items) {
                std::string old_key = make_index_key_from_record(index.cols, item.old_record->data);
                std::string new_key = make_index_key_from_record(index.cols, item.new_record->data);
                if (!new_keys.insert(new_key).second) {
                    throw InternalError("Duplicate index key");
                }
                if (old_key == new_key) {
                    continue;
                }
                std::vector<Rid> exists;
                if (ih->get_value(new_key.data(), &exists, context_->txn_)) {
                    bool self = false;
                    for (auto &exist_rid : exists) {
                        if (exist_rid == item.rid) {
                            self = true;
                        }
                    }
                    if (!self) {
                        throw InternalError("Duplicate index key");
                    }
                }
            }
        }

        for (auto &item : items) {
            UpdateLogRecord log_record(context_->txn_->get_transaction_id(), *item.old_record, *item.new_record,
                                       item.rid, tab_name_);
            append_txn_log(context_, &log_record);
            context_->txn_->append_write_record(
                new WriteRecord(WType::UPDATE_TUPLE, tab_name_, item.rid, *item.old_record));
            for (auto &index : tab_.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                std::string old_key = make_index_key_from_record(index.cols, item.old_record->data);
                std::string new_key = make_index_key_from_record(index.cols, item.new_record->data);
                if (old_key != new_key) {
                    ih->delete_entry(old_key.data(), context_->txn_);
                    ih->insert_entry(new_key.data(), item.rid, context_->txn_);
                }
            }
            fh_->update_record(item.rid, item.new_record->data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
