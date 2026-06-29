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

#include "executor_abstract.h"

class LimitExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    size_t limit_;
    size_t emitted_ = 0;
    bool is_end_ = true;

   public:
    LimitExecutor(std::unique_ptr<AbstractExecutor> prev, size_t limit) {
        prev_ = std::move(prev);
        limit_ = limit;
    }

    void beginTuple() override {
        emitted_ = 0;
        prev_->beginTuple();
        is_end_ = limit_ == 0 || prev_->is_end();
    }

    void nextTuple() override {
        if (is_end_) {
            return;
        }
        ++emitted_;
        if (emitted_ >= limit_) {
            is_end_ = true;
            return;
        }
        prev_->nextTuple();
        is_end_ = prev_->is_end();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return prev_->Next();
    }

    bool is_end() const override { return is_end_; }

    size_t tupleLen() const override { return prev_->tupleLen(); }

    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }

    ColMeta get_col_offset(const TabCol &target) override { return prev_->get_col_offset(target); }

    Rid &rid() override { return prev_->rid(); }
};
