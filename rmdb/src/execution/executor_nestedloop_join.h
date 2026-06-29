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

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    static constexpr size_t JOIN_BUFFER_BYTES = 64 * 1024 * 1024;

    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;

    std::vector<Condition> fed_conds_;
    bool isend;
    std::unique_ptr<RmRecord> current_tuple_;
    std::vector<std::unique_ptr<RmRecord>> left_block_;
    size_t left_block_idx_ = 0;
    size_t left_block_bytes_ = 0;
    std::unique_ptr<RmRecord> right_tuple_;

    std::unique_ptr<RmRecord> make_join_record(const RmRecord &left_record, const RmRecord &right_record) {
        auto record = std::make_unique<RmRecord>(len_);
        memcpy(record->data, left_record.data, left_->tupleLen());
        memcpy(record->data + left_->tupleLen(), right_record.data, right_->tupleLen());
        return record;
    }

    size_t buffered_record_bytes(const RmRecord &record) const {
        return static_cast<size_t>(record.size) + sizeof(RmRecord) + sizeof(std::unique_ptr<RmRecord>);
    }

    bool load_left_block() {
        left_block_.clear();
        left_block_idx_ = 0;
        left_block_bytes_ = 0;

        while (!left_->is_end()) {
            auto record = left_->Next();
            left_->nextTuple();
            if (record == nullptr) {
                continue;
            }

            size_t record_bytes = buffered_record_bytes(*record);
            if (!left_block_.empty() && left_block_bytes_ + record_bytes > JOIN_BUFFER_BYTES) {
                break;
            }
            left_block_bytes_ += record_bytes;
            left_block_.push_back(std::move(record));
            if (left_block_bytes_ >= JOIN_BUFFER_BYTES) {
                break;
            }
        }

        return !left_block_.empty();
    }

    void reset_right_scan() {
        right_->beginTuple();
        right_tuple_.reset();
        left_block_idx_ = 0;
    }

    void advance_to_valid_record() {
        current_tuple_ = nullptr;
        while (!left_block_.empty()) {
            while (!right_->is_end()) {
                if (right_tuple_ == nullptr) {
                    right_tuple_ = right_->Next();
                    left_block_idx_ = 0;
                }
                if (right_tuple_ == nullptr) {
                    right_->nextTuple();
                    continue;
                }

                while (left_block_idx_ < left_block_.size()) {
                    auto record = make_join_record(*left_block_[left_block_idx_], *right_tuple_);
                    ++left_block_idx_;
                    if (eval_conditions(cols_, *record, fed_conds_)) {
                        current_tuple_ = std::move(record);
                        isend = false;
                        return;
                    }
                }

                right_tuple_.reset();
                right_->nextTuple();
            }

            if (!load_left_block()) {
                isend = true;
                return;
            }
            reset_right_scan();
        }
        isend = true;
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
    }

    void beginTuple() override {
        left_->beginTuple();
        right_tuple_.reset();
        current_tuple_ = nullptr;
        if (!load_left_block()) {
            isend = true;
            return;
        }
        reset_right_scan();
        advance_to_valid_record();
    }

    void nextTuple() override {
        if (isend) {
            return;
        }
        advance_to_valid_record();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (current_tuple_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_tuple_);
    }

    bool is_end() const override { return isend; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return _abstract_rid; }
};
