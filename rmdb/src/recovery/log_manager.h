/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "log_defs.h"
#include "record/rm_defs.h"

enum LogType : int { UPDATE = 0, INSERT, DELETE, begin, commit, ABORT };

static std::string LogTypeStr[] = {"UPDATE", "INSERT", "DELETE", "BEGIN", "COMMIT", "ABORT"};

class LogRecord {
   public:
    LogType log_type_;
    lsn_t lsn_;
    uint32_t log_tot_len_;
    txn_id_t log_tid_;
    lsn_t prev_lsn_;

    virtual ~LogRecord() = default;

    virtual void serialize(char* dest) const {
        memcpy(dest + OFFSET_LOG_TYPE, &log_type_, sizeof(LogType));
        memcpy(dest + OFFSET_LSN, &lsn_, sizeof(lsn_t));
        memcpy(dest + OFFSET_LOG_TOT_LEN, &log_tot_len_, sizeof(uint32_t));
        memcpy(dest + OFFSET_LOG_TID, &log_tid_, sizeof(txn_id_t));
        memcpy(dest + OFFSET_PREV_LSN, &prev_lsn_, sizeof(lsn_t));
    }

    virtual void deserialize(const char* src) {
        log_type_ = *reinterpret_cast<const LogType*>(src + OFFSET_LOG_TYPE);
        lsn_ = *reinterpret_cast<const lsn_t*>(src + OFFSET_LSN);
        log_tot_len_ = *reinterpret_cast<const uint32_t*>(src + OFFSET_LOG_TOT_LEN);
        log_tid_ = *reinterpret_cast<const txn_id_t*>(src + OFFSET_LOG_TID);
        prev_lsn_ = *reinterpret_cast<const lsn_t*>(src + OFFSET_PREV_LSN);
    }

    virtual void format_print() {
        std::cout << "log_type: " << LogTypeStr[log_type_] << "\n";
        std::cout << "lsn: " << lsn_ << "\n";
        std::cout << "log_tot_len: " << log_tot_len_ << "\n";
        std::cout << "log_tid: " << log_tid_ << "\n";
        std::cout << "prev_lsn: " << prev_lsn_ << "\n";
    }
};

class BeginLogRecord : public LogRecord {
   public:
    BeginLogRecord() {
        log_type_ = LogType::begin;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }

    explicit BeginLogRecord(txn_id_t txn_id) : BeginLogRecord() { log_tid_ = txn_id; }
};

class CommitLogRecord : public LogRecord {
   public:
    CommitLogRecord() {
        log_type_ = LogType::commit;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }

    explicit CommitLogRecord(txn_id_t txn_id) : CommitLogRecord() { log_tid_ = txn_id; }
};

class AbortLogRecord : public LogRecord {
   public:
    AbortLogRecord() {
        log_type_ = LogType::ABORT;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }

    explicit AbortLogRecord(txn_id_t txn_id) : AbortLogRecord() { log_tid_ = txn_id; }
};

class TableRecordLogRecord : public LogRecord {
   public:
    char* table_name_ = nullptr;
    size_t table_name_size_ = 0;

    ~TableRecordLogRecord() override {
        delete[] table_name_;
        table_name_ = nullptr;
    }

    std::string table_name() const { return std::string(table_name_, table_name_size_); }

   protected:
    void set_table_name(const std::string& table_name) {
        table_name_size_ = table_name.length();
        table_name_ = new char[table_name_size_];
        memcpy(table_name_, table_name.data(), table_name_size_);
    }

    void serialize_table_name(char* dest, int& offset) const {
        memcpy(dest + offset, &table_name_size_, sizeof(size_t));
        offset += sizeof(size_t);
        memcpy(dest + offset, table_name_, table_name_size_);
        offset += static_cast<int>(table_name_size_);
    }

    void deserialize_table_name(const char* src, int& offset) {
        table_name_size_ = *reinterpret_cast<const size_t*>(src + offset);
        offset += sizeof(size_t);
        delete[] table_name_;
        table_name_ = new char[table_name_size_];
        memcpy(table_name_, src + offset, table_name_size_);
        offset += static_cast<int>(table_name_size_);
    }
};

class InsertLogRecord : public TableRecordLogRecord {
   public:
    InsertLogRecord() {
        log_type_ = LogType::INSERT;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }

    InsertLogRecord(txn_id_t txn_id, RmRecord& insert_value, Rid& rid, std::string table_name) : InsertLogRecord() {
        log_tid_ = txn_id;
        insert_value_ = insert_value;
        rid_ = rid;
        set_table_name(table_name);
        log_tot_len_ += sizeof(int) + insert_value_.size;
        log_tot_len_ += sizeof(Rid);
        log_tot_len_ += sizeof(size_t) + table_name_size_;
    }

    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        memcpy(dest + offset, &insert_value_.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, insert_value_.data, insert_value_.size);
        offset += insert_value_.size;
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        serialize_table_name(dest, offset);
    }

    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        insert_value_.Deserialize(src + OFFSET_LOG_DATA);
        int offset = OFFSET_LOG_DATA + sizeof(int) + insert_value_.size;
        rid_ = *reinterpret_cast<const Rid*>(src + offset);
        offset += sizeof(Rid);
        deserialize_table_name(src, offset);
    }

    RmRecord insert_value_;
    Rid rid_;
};

class DeleteLogRecord : public TableRecordLogRecord {
   public:
    DeleteLogRecord() {
        log_type_ = LogType::DELETE;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }

    DeleteLogRecord(txn_id_t txn_id, RmRecord& delete_value, Rid& rid, std::string table_name) : DeleteLogRecord() {
        log_tid_ = txn_id;
        delete_value_ = delete_value;
        rid_ = rid;
        set_table_name(table_name);
        log_tot_len_ += sizeof(int) + delete_value_.size;
        log_tot_len_ += sizeof(Rid);
        log_tot_len_ += sizeof(size_t) + table_name_size_;
    }

    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        memcpy(dest + offset, &delete_value_.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, delete_value_.data, delete_value_.size);
        offset += delete_value_.size;
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        serialize_table_name(dest, offset);
    }

    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        delete_value_.Deserialize(src + OFFSET_LOG_DATA);
        int offset = OFFSET_LOG_DATA + sizeof(int) + delete_value_.size;
        rid_ = *reinterpret_cast<const Rid*>(src + offset);
        offset += sizeof(Rid);
        deserialize_table_name(src, offset);
    }

    RmRecord delete_value_;
    Rid rid_;
};

class UpdateLogRecord : public TableRecordLogRecord {
   public:
    UpdateLogRecord() {
        log_type_ = LogType::UPDATE;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }

    UpdateLogRecord(txn_id_t txn_id, RmRecord& old_value, RmRecord& new_value, Rid& rid, std::string table_name)
        : UpdateLogRecord() {
        log_tid_ = txn_id;
        old_value_ = old_value;
        new_value_ = new_value;
        rid_ = rid;
        set_table_name(table_name);
        log_tot_len_ += sizeof(int) + old_value_.size;
        log_tot_len_ += sizeof(int) + new_value_.size;
        log_tot_len_ += sizeof(Rid);
        log_tot_len_ += sizeof(size_t) + table_name_size_;
    }

    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        memcpy(dest + offset, &old_value_.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, old_value_.data, old_value_.size);
        offset += old_value_.size;
        memcpy(dest + offset, &new_value_.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, new_value_.data, new_value_.size);
        offset += new_value_.size;
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        serialize_table_name(dest, offset);
    }

    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        old_value_.Deserialize(src + OFFSET_LOG_DATA);
        int offset = OFFSET_LOG_DATA + sizeof(int) + old_value_.size;
        new_value_.Deserialize(src + offset);
        offset += sizeof(int) + new_value_.size;
        rid_ = *reinterpret_cast<const Rid*>(src + offset);
        offset += sizeof(Rid);
        deserialize_table_name(src, offset);
    }

    RmRecord old_value_;
    RmRecord new_value_;
    Rid rid_;
};

class LogBuffer {
   public:
    LogBuffer() {
        offset_ = 0;
        memset(buffer_, 0, sizeof(buffer_));
    }

    bool is_full(int append_size) { return offset_ + append_size > LOG_BUFFER_SIZE; }

    char buffer_[LOG_BUFFER_SIZE + 1];
    int offset_;
};

class LogManager {
   public:
    explicit LogManager(DiskManager* disk_manager) {
        disk_manager_ = disk_manager;
        persist_lsn_ = INVALID_LSN;
    }

    lsn_t add_log_to_buffer(LogRecord* log_record);
    void flush_log_to_disk();

    LogBuffer* get_log_buffer() { return &log_buffer_; }

   private:
    std::atomic<lsn_t> global_lsn_{0};
    std::mutex latch_;
    LogBuffer log_buffer_;
    lsn_t persist_lsn_;
    DiskManager* disk_manager_;
};
