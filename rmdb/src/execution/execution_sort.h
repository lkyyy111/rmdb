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

#include <algorithm>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
    private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> sort_cols_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<bool> is_descs_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t pos_ = 0;
    bool is_end_ = true;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols,
                 std::vector<bool> is_descs) {
        prev_ = std::move(prev);
        for (auto &sel_col : sel_cols) {
            sort_cols_.push_back(prev_->get_col_offset(sel_col));
        }
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
        is_descs_ = std::move(is_descs);
    }

    void beginTuple() override {
        tuples_.clear();
        prev_->beginTuple();
        for (; !prev_->is_end(); prev_->nextTuple()) {
            auto record = prev_->Next();
            if (record != nullptr) {
                tuples_.push_back(std::move(record));
            }
        }

        std::stable_sort(tuples_.begin(), tuples_.end(), [&](const auto &lhs, const auto &rhs) {
            for (size_t i = 0; i < sort_cols_.size(); ++i) {
                auto &sort_col = sort_cols_[i];
                int cmp = compare_raw_value(lhs->data + sort_col.offset, rhs->data + sort_col.offset,
                                            sort_col.type, sort_col.len);
                if (cmp != 0) {
                    return is_descs_[i] ? cmp > 0 : cmp < 0;
                }
            }
            return false;
        });
        pos_ = 0;
        is_end_ = tuples_.empty();
    }

    void nextTuple() override {
        if (is_end_) {
            return;
        }
        ++pos_;
        is_end_ = pos_ >= tuples_.size();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*tuples_[pos_]);
    }

    bool is_end() const override { return is_end_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return _abstract_rid; }
};
