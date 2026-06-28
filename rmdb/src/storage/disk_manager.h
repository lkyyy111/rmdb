/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

#include "common/config.h"
#include "errors.h"

class DiskManager {
   public:
    explicit DiskManager();
    ~DiskManager();

    void write_page(int fd, page_id_t page_no, const char *offset, int num_bytes);
    void read_page(int fd, page_id_t page_no, char *offset, int num_bytes);

    page_id_t allocate_page(int fd);
    void deallocate_page(page_id_t page_id);

    bool is_dir(const std::string &path);
    void create_dir(const std::string &path);
    void destroy_dir(const std::string &path);

    bool is_file(const std::string &path);
    void create_file(const std::string &path);
    void destroy_file(const std::string &path);
    int open_file(const std::string &path);
    void close_file(int fd);

    int get_file_size(const std::string &file_name);
    std::string get_file_name(int fd);
    int get_file_fd(const std::string &file_name);

    int read_log(char *log_data, int size, int offset);
    void write_log(char *log_data, int size);

    void SetLogFd(int log_fd) { log_fd_ = log_fd; }
    int GetLogFd() { return log_fd_; }

    void set_fd2pageno(int fd, int start_page_no) { fd2pageno_[fd] = start_page_no; }
    page_id_t get_fd2pageno(int fd) { return fd2pageno_[fd]; }

    static constexpr int MAX_FD = 8192;

   private:
    std::unordered_map<std::string, int> path2fd_;
    std::unordered_map<int, std::string> fd2path_;
    std::unordered_map<std::string, int> closed_path2fd_;

    int log_fd_ = -1;
    std::atomic<page_id_t> fd2pageno_[MAX_FD]{};
};
