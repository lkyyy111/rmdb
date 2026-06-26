/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "ix_index_handle.h"

#include <algorithm>

#include "ix_scan.h"

int IxNodeHandle::lower_bound(const char *target) const {
    int left = 0;
    int right = get_size();
    while (left < right) {
        int mid = left + (right - left) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

int IxNodeHandle::upper_bound(const char *target) const {
    int left = 0;
    int right = get_size();
    while (left < right) {
        int mid = left + (right - left) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        *value = get_rid(pos);
        return true;
    }
    return false;
}

page_id_t IxNodeHandle::internal_lookup(const char *key) {
    int pos = upper_bound(key) - 1;
    if (pos < 0) {
        pos = 0;
    }
    return value_at(pos);
}

void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    assert(pos >= 0 && pos <= get_size());
    assert(n >= 0 && get_size() + n <= get_max_size());
    int old_size = get_size();
    int key_len = file_hdr->col_tot_len_;
    memmove(get_key(pos + n), get_key(pos), (old_size - pos) * key_len);
    memmove(get_rid(pos + n), get_rid(pos), (old_size - pos) * sizeof(Rid));
    memcpy(get_key(pos), key, n * key_len);
    memcpy(get_rid(pos), rid, n * sizeof(Rid));
    set_size(old_size + n);
}

int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        return get_size();
    }
    insert_pair(pos, key, value);
    return get_size();
}

void IxNodeHandle::erase_pair(int pos) {
    assert(pos >= 0 && pos < get_size());
    int old_size = get_size();
    int key_len = file_hdr->col_tot_len_;
    memmove(get_key(pos), get_key(pos + 1), (old_size - pos - 1) * key_len);
    memmove(get_rid(pos), get_rid(pos + 1), (old_size - pos - 1) * sizeof(Rid));
    set_size(old_size - 1);
}

int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        erase_pair(pos);
    }
    return get_size();
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    char *buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;

    int now_page_no = disk_manager_->get_fd2pageno(fd);
    disk_manager_->set_fd2pageno(fd, now_page_no + 1);
}

int IxIndexHandle::compare_key(const std::string &lhs, const char *rhs) const {
    return ix_compare(lhs.data(), rhs, file_hdr_->col_types_, file_hdr_->col_lens_);
}

int IxIndexHandle::lower_bound_pos(const char *key) const {
    int left = 0;
    int right = static_cast<int>(mem_entries_.size());
    while (left < right) {
        int mid = left + (right - left) / 2;
        if (compare_key(mem_entries_[mid].first, key) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

int IxIndexHandle::upper_bound_pos(const char *key) const {
    int left = 0;
    int right = static_cast<int>(mem_entries_.size());
    while (left < right) {
        int mid = left + (right - left) / 2;
        if (compare_key(mem_entries_[mid].first, key) <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                              Transaction *transaction, bool find_first) {
    (void)key;
    (void)operation;
    (void)transaction;
    (void)find_first;
    return std::make_pair(nullptr, false);
}

bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    (void)transaction;
    int pos = lower_bound_pos(key);
    if (pos < static_cast<int>(mem_entries_.size()) && compare_key(mem_entries_[pos].first, key) == 0) {
        result->push_back(mem_entries_[pos].second);
        return true;
    }
    return false;
}

IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    (void)node;
    return nullptr;
}

void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                       Transaction *transaction) {
    (void)old_node;
    (void)key;
    (void)new_node;
    (void)transaction;
}

page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    (void)transaction;
    int pos = lower_bound_pos(key);
    if (pos < static_cast<int>(mem_entries_.size()) && compare_key(mem_entries_[pos].first, key) == 0) {
        throw InternalError("Duplicate index key");
    }
    mem_entries_.insert(mem_entries_.begin() + pos, {std::string(key, file_hdr_->col_tot_len_), value});
    return 0;
}

bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    (void)transaction;
    int pos = lower_bound_pos(key);
    if (pos < static_cast<int>(mem_entries_.size()) && compare_key(mem_entries_[pos].first, key) == 0) {
        mem_entries_.erase(mem_entries_.begin() + pos);
        return true;
    }
    return false;
}

bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    (void)node;
    (void)transaction;
    (void)root_is_latched;
    return false;
}

bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    (void)old_root_node;
    return false;
}

void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    (void)neighbor_node;
    (void)node;
    (void)parent;
    (void)index;
}

bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    (void)neighbor_node;
    (void)node;
    (void)parent;
    (void)index;
    (void)transaction;
    (void)root_is_latched;
    return false;
}

Rid IxIndexHandle::get_rid(const Iid &iid) const {
    if (iid.page_no == 0) {
        if (iid.slot_no < 0 || iid.slot_no >= static_cast<int>(mem_entries_.size())) {
            throw IndexEntryNotFoundError();
        }
        return mem_entries_[iid.slot_no].second;
    }
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        throw IndexEntryNotFoundError();
    }
    Rid rid = *node->get_rid(iid.slot_no);
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    return rid;
}

Iid IxIndexHandle::lower_bound(const char *key) {
    return Iid{0, lower_bound_pos(key)};
}

Iid IxIndexHandle::upper_bound(const char *key) {
    return Iid{0, upper_bound_pos(key)};
}

Iid IxIndexHandle::leaf_end() const {
    return Iid{0, static_cast<int>(mem_entries_.size())};
}

Iid IxIndexHandle::leaf_begin() const {
    return Iid{0, 0};
}

void IxIndexHandle::scan_range(const char *lower_key, bool has_lower, bool lower_inclusive,
                               const char *upper_key, bool has_upper, bool upper_inclusive,
                               std::vector<Rid> *result) const {
    int begin = has_lower ? (lower_inclusive ? lower_bound_pos(lower_key) : upper_bound_pos(lower_key)) : 0;
    int end = has_upper ? (upper_inclusive ? upper_bound_pos(upper_key) : lower_bound_pos(upper_key))
                        : static_cast<int>(mem_entries_.size());
    begin = std::max(begin, 0);
    end = std::min(end, static_cast<int>(mem_entries_.size()));
    for (int i = begin; i < end; ++i) {
        result->push_back(mem_entries_[i].second);
    }
}

IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
    return node;
}

IxNodeHandle *IxIndexHandle::create_node() {
    file_hdr_->num_pages_++;
    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    return new IxNodeHandle(file_hdr_, page);
}

void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE) {
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        char *child_first_key = curr->get_key(0);
        if (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) == 0) {
            assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
            break;
        }
        memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);
        curr = parent;
        assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
    }
}

void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());
    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);

    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
}

void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    (void)node;
    file_hdr_->num_pages_--;
}

void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    }
}
