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

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include "defs.h"
#include "record/rm_defs.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

inline bool is_datetime_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

inline int datetime_days_in_month(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_datetime_leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

inline int parse_datetime_part(const std::string &str, int pos, int len) {
    int value = 0;
    for (int i = 0; i < len; ++i) {
        char ch = str[pos + i];
        if (ch < '0' || ch > '9') {
            return -1;
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

inline bool parse_datetime_literal(const std::string &str, std::int64_t *datetime_val) {
    if (str.size() != 19 || str[4] != '-' || str[7] != '-' ||
        str[10] != ' ' || str[13] != ':' || str[16] != ':') {
        return false;
    }

    int year = parse_datetime_part(str, 0, 4);
    int month = parse_datetime_part(str, 5, 2);
    int day = parse_datetime_part(str, 8, 2);
    int hour = parse_datetime_part(str, 11, 2);
    int minute = parse_datetime_part(str, 14, 2);
    int second = parse_datetime_part(str, 17, 2);
    if (year < 1000 || year > 9999 || month < 1 || month > 12 ||
        day < 1 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return false;
    }
    if (day > datetime_days_in_month(year, month)) {
        return false;
    }

    if (datetime_val != nullptr) {
        *datetime_val = static_cast<std::int64_t>(year) * 10000000000LL +
                        static_cast<std::int64_t>(month) * 100000000LL +
                        static_cast<std::int64_t>(day) * 1000000LL +
                        static_cast<std::int64_t>(hour) * 10000LL +
                        static_cast<std::int64_t>(minute) * 100LL + second;
    }
    return true;
}

inline std::string datetime_to_string(std::int64_t datetime_val) {
    int second = static_cast<int>(datetime_val % 100);
    datetime_val /= 100;
    int minute = static_cast<int>(datetime_val % 100);
    datetime_val /= 100;
    int hour = static_cast<int>(datetime_val % 100);
    datetime_val /= 100;
    int day = static_cast<int>(datetime_val % 100);
    datetime_val /= 100;
    int month = static_cast<int>(datetime_val % 100);
    datetime_val /= 100;
    int year = static_cast<int>(datetime_val);

    char buf[20];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  year, month, day, hour, minute, second);
    return std::string(buf);
}

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        std::int64_t bigint_val;  // bigint value
        std::int64_t datetime_val;  // datetime value
        float float_val;  // float value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        raw.reset();
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        raw.reset();
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_bigint(std::int64_t bigint_val_) {
        raw.reset();
        type = TYPE_BIGINT;
        bigint_val = bigint_val_;
    }

    void set_datetime(std::int64_t datetime_val_) {
        raw.reset();
        type = TYPE_DATETIME;
        datetime_val = datetime_val_;
    }

    void set_str(std::string str_val_) {
        raw.reset();
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_BIGINT) {
            assert(len == sizeof(std::int64_t));
            *(std::int64_t *)(raw->data) = bigint_val;
        } else if (type == TYPE_DATETIME) {
            assert(len == sizeof(std::int64_t));
            *(std::int64_t *)(raw->data) = datetime_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

inline bool coerce_value_to_col_type(Value &val, ColType target_type) {
    if (val.type == target_type) {
        return true;
    }
    if (target_type == TYPE_BIGINT && val.type == TYPE_INT) {
        val.set_bigint(val.int_val);
        return true;
    }
    if (target_type == TYPE_INT && val.type == TYPE_BIGINT) {
        if (val.bigint_val < std::numeric_limits<int>::min() ||
            val.bigint_val > std::numeric_limits<int>::max()) {
            return false;
        }
        val.set_int(static_cast<int>(val.bigint_val));
        return true;
    }
    if (target_type == TYPE_FLOAT && val.type == TYPE_INT) {
        val.set_float(static_cast<float>(val.int_val));
        return true;
    }
    if (target_type == TYPE_FLOAT && val.type == TYPE_BIGINT) {
        val.set_float(static_cast<float>(val.bigint_val));
        return true;
    }
    if (target_type == TYPE_DATETIME && val.type == TYPE_STRING) {
        std::int64_t datetime_val;
        if (!parse_datetime_literal(val.str_val, &datetime_val)) {
            return false;
        }
        val.set_datetime(datetime_val);
        return true;
    }
    return false;
}

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

struct SetClause {
    TabCol lhs;
    Value rhs;
};
