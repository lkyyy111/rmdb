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
#include <cstdint>
#include <cmath>

#include "execution_defs.h"
#include "common/common.h"
#include "index/ix.h"
#include "system/sm.h"

inline int compare_raw_value(const char *lhs, const char *rhs, ColType type, int len) {
    switch (type) {
        case TYPE_INT: {
            int lhs_val = *reinterpret_cast<const int *>(lhs);
            int rhs_val = *reinterpret_cast<const int *>(rhs);
            return (lhs_val > rhs_val) - (lhs_val < rhs_val);
        }
        case TYPE_BIGINT: {
            std::int64_t lhs_val = *reinterpret_cast<const std::int64_t *>(lhs);
            std::int64_t rhs_val = *reinterpret_cast<const std::int64_t *>(rhs);
            return (lhs_val > rhs_val) - (lhs_val < rhs_val);
        }
        case TYPE_DATETIME: {
            std::int64_t lhs_val = *reinterpret_cast<const std::int64_t *>(lhs);
            std::int64_t rhs_val = *reinterpret_cast<const std::int64_t *>(rhs);
            return (lhs_val > rhs_val) - (lhs_val < rhs_val);
        }
        case TYPE_FLOAT: {
            float lhs_val = *reinterpret_cast<const float *>(lhs);
            float rhs_val = *reinterpret_cast<const float *>(rhs);
            float diff = lhs_val - rhs_val;
            if (std::fabs(diff) < 1e-6f) {
                return 0;
            }
            return diff > 0 ? 1 : -1;
        }
        case TYPE_STRING:
            return memcmp(lhs, rhs, len);
    }
    throw InternalError("Unexpected field type");
}

inline bool compare_with_op(int cmp, CompOp op) {
    switch (op) {
        case OP_EQ:
            return cmp == 0;
        case OP_NE:
            return cmp != 0;
        case OP_LT:
            return cmp < 0;
        case OP_GT:
            return cmp > 0;
        case OP_LE:
            return cmp <= 0;
        case OP_GE:
            return cmp >= 0;
    }
    throw InternalError("Unexpected comparison op");
}

inline std::vector<ColMeta>::const_iterator find_col_meta(const std::vector<ColMeta> &cols, const TabCol &target) {
    return std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
        return col.tab_name == target.tab_name && col.name == target.col_name;
    });
}

inline bool eval_condition(const std::vector<ColMeta> &cols, const RmRecord &record, const Condition &cond) {
    auto lhs_col = find_col_meta(cols, cond.lhs_col);
    if (lhs_col == cols.end()) {
        throw ColumnNotFoundError(cond.lhs_col.tab_name + "." + cond.lhs_col.col_name);
    }

    const char *lhs = record.data + lhs_col->offset;
    const char *rhs = nullptr;
    std::shared_ptr<RmRecord> rhs_holder;
    if (cond.is_rhs_val) {
        if (cond.rhs_val.raw == nullptr) {
            Value rhs_val = cond.rhs_val;
            rhs_val.init_raw(lhs_col->len);
            rhs_holder = rhs_val.raw;
            rhs = rhs_holder->data;
        } else {
            rhs = cond.rhs_val.raw->data;
        }
    } else {
        auto rhs_col = find_col_meta(cols, cond.rhs_col);
        if (rhs_col == cols.end()) {
            throw ColumnNotFoundError(cond.rhs_col.tab_name + "." + cond.rhs_col.col_name);
        }
        rhs = record.data + rhs_col->offset;
    }
    return compare_with_op(compare_raw_value(lhs, rhs, lhs_col->type, lhs_col->len), cond.op);
}

inline bool eval_conditions(const std::vector<ColMeta> &cols, const RmRecord &record,
                            const std::vector<Condition> &conds) {
    for (auto &cond : conds) {
        if (!eval_condition(cols, record, cond)) {
            return false;
        }
    }
    return true;
}

class AbstractExecutor {
   public:
    Rid _abstract_rid;

    Context *context_;

    virtual ~AbstractExecutor() = default;

    virtual size_t tupleLen() const { return 0; };

    virtual const std::vector<ColMeta> &cols() const {
        std::vector<ColMeta> *_cols = nullptr;
        return *_cols;
    };

    virtual std::string getType() { return "AbstractExecutor"; };

    virtual void beginTuple(){};

    virtual void nextTuple(){};

    virtual bool is_end() const { return true; };

    virtual Rid &rid() = 0;

    virtual std::unique_ptr<RmRecord> Next() = 0;

    virtual ColMeta get_col_offset(const TabCol &target) { return ColMeta();};

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }
};
