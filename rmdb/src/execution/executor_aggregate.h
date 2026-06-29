/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the
Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "executor_abstract.h"
#include "parser/ast.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<std::shared_ptr<ast::AggExpr>> aggs_;
    std::vector<ColMeta> input_cols_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    bool is_end_ = true;
    std::unique_ptr<RmRecord> result_;

    static int type_len(ColType type) {
        switch (type) {
            case TYPE_INT:
                return sizeof(int);
            case TYPE_FLOAT:
                return sizeof(float);
            case TYPE_BIGINT:
            case TYPE_DATETIME:
                return sizeof(std::int64_t);
            case TYPE_STRING:
                break;
        }
        throw InternalError("Unexpected aggregate result type");
    }

    void build_output_cols() {
        for (auto &agg : aggs_) {
            ColMeta input_col{};
            if (!agg->is_star) {
                input_col = prev_->get_col_offset(TabCol{agg->col->tab_name, agg->col->col_name});
            }
            input_cols_.push_back(input_col);

            ColMeta out_col{};
            out_col.tab_name = "";
            out_col.name = agg->alias;
            out_col.offset = static_cast<int>(len_);
            out_col.index = false;

            if (agg->type == ast::AGG_COUNT) {
                out_col.type = TYPE_INT;
                out_col.len = sizeof(int);
            } else if (agg->type == ast::AGG_SUM) {
                if (input_col.type != TYPE_INT && input_col.type != TYPE_FLOAT && input_col.type != TYPE_BIGINT) {
                    throw IncompatibleTypeError("INT/FLOAT", coltype2str(input_col.type));
                }
                out_col.type = input_col.type;
                out_col.len = type_len(out_col.type);
            } else {
                out_col.type = input_col.type;
                out_col.len = input_col.len;
            }

            len_ += out_col.len;
            cols_.push_back(out_col);
        }
    }

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev,
                      std::vector<std::shared_ptr<ast::AggExpr>> aggs) {
        prev_ = std::move(prev);
        aggs_ = std::move(aggs);
        build_output_cols();
    }

    void beginTuple() override {
        result_ = std::make_unique<RmRecord>(static_cast<int>(len_));
        memset(result_->data, 0, len_);

        std::vector<bool> has_value(aggs_.size(), false);
        std::vector<int> counts(aggs_.size(), 0);
        std::vector<std::int64_t> int_sums(aggs_.size(), 0);
        std::vector<double> float_sums(aggs_.size(), 0.0);

        prev_->beginTuple();
        for (; !prev_->is_end(); prev_->nextTuple()) {
            auto record = prev_->Next();
            if (record == nullptr) {
                continue;
            }

            for (size_t i = 0; i < aggs_.size(); ++i) {
                auto &agg = aggs_[i];
                auto &out_col = cols_[i];
                char *dst = result_->data + out_col.offset;

                if (agg->type == ast::AGG_COUNT) {
                    ++counts[i];
                    continue;
                }

                auto &input_col = input_cols_[i];
                const char *src = record->data + input_col.offset;
                if (agg->type == ast::AGG_SUM) {
                    if (input_col.type == TYPE_FLOAT) {
                        float_sums[i] += *reinterpret_cast<const float *>(src);
                    } else if (input_col.type == TYPE_BIGINT) {
                        int_sums[i] += *reinterpret_cast<const std::int64_t *>(src);
                    } else {
                        int_sums[i] += *reinterpret_cast<const int *>(src);
                    }
                    continue;
                }

                if (!has_value[i]) {
                    memcpy(dst, src, input_col.len);
                    has_value[i] = true;
                    continue;
                }

                int cmp = compare_raw_value(src, dst, input_col.type, input_col.len);
                if ((agg->type == ast::AGG_MAX && cmp > 0) || (agg->type == ast::AGG_MIN && cmp < 0)) {
                    memcpy(dst, src, input_col.len);
                }
            }
        }

        for (size_t i = 0; i < aggs_.size(); ++i) {
            auto &agg = aggs_[i];
            auto &out_col = cols_[i];
            char *dst = result_->data + out_col.offset;
            if (agg->type == ast::AGG_COUNT) {
                *reinterpret_cast<int *>(dst) = counts[i];
            } else if (agg->type == ast::AGG_SUM) {
                if (out_col.type == TYPE_FLOAT) {
                    *reinterpret_cast<float *>(dst) = static_cast<float>(float_sums[i]);
                } else if (out_col.type == TYPE_BIGINT) {
                    *reinterpret_cast<std::int64_t *>(dst) = int_sums[i];
                } else {
                    *reinterpret_cast<int *>(dst) = static_cast<int>(int_sums[i]);
                }
            }
        }

        is_end_ = false;
    }

    void nextTuple() override { is_end_ = true; }

    bool is_end() const override { return is_end_; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_ || result_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*result_);
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return _abstract_rid; }
};
