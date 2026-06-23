#include "minidb/storage/buffer_pool.h"

#include "minidb/exceptions.h"

namespace minidb {

BufferPool::BufferPool(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) capacity_ = 1;
}

int BufferPool::register_file(DiskManager* dm) {
    files_.push_back(dm);
    return static_cast<int>(files_.size()) - 1;
}

void BufferPool::register_file_with_id(int file_id, DiskManager* dm) {
    if (static_cast<int>(files_.size()) <= file_id) {
        files_.resize(file_id + 1, nullptr);
    }
    files_[file_id] = dm;
}

page_id_t BufferPool::file_page_count(int file_id) const {
    return files_.at(file_id)->num_pages();
}

Page* BufferPool::fetch_page(int file_id, page_id_t page_id) {
    uint64_t key = make_key(file_id, page_id);
    auto it = page_table_.find(key);
    if (it != page_table_.end()) {
        // Cache hit. Pin it and remove it from the LRU list (pinned frames are
        // not eviction candidates).
        Frame* frame = it->second.get();
        if (frame->pin_count == 0) {
            auto pos = lru_pos_.find(key);
            if (pos != lru_pos_.end()) {
                lru_.erase(pos->second);
                lru_pos_.erase(pos);
            }
        }
        frame->pin_count++;
        ++hits_;
        return &frame->page;
    }

    // Cache miss: make room, then read the page from disk into a new frame.
    ++misses_;
    if (page_table_.size() >= capacity_) {
        evict_one();
    }
    auto frame = std::make_unique<Frame>();
    frame->file_id = file_id;
    frame->page_id = page_id;
    frame->pin_count = 1;
    frame->dirty = false;
    std::vector<uint8_t> raw;
    files_.at(file_id)->read_page(page_id, raw);
    frame->page = Page(raw);
    Page* result = &frame->page;
    page_table_[key] = std::move(frame);
    return result;
}

Page* BufferPool::new_page(int file_id, page_id_t* out_page_id) {
    page_id_t pid = files_.at(file_id)->allocate_page();
    if (out_page_id) *out_page_id = pid;
    // The page was zero-filled on disk; fetch it so it lives in a frame, then
    // overwrite it with a properly initialised (empty slotted) page.
    Page* page = fetch_page(file_id, pid);
    *page = Page();  // reset header to an empty slotted page
    return page;
}

void BufferPool::unpin_page(int file_id, page_id_t page_id, bool is_dirty) {
    uint64_t key = make_key(file_id, page_id);
    auto it = page_table_.find(key);
    if (it == page_table_.end()) return;
    Frame* frame = it->second.get();
    if (is_dirty) frame->dirty = true;
    if (frame->pin_count > 0) frame->pin_count--;
    if (frame->pin_count == 0) {
        // Newly evictable: append to the back of the LRU list (most recent).
        lru_.push_back(key);
        lru_pos_[key] = std::prev(lru_.end());
    }
}

void BufferPool::write_frame_to_disk(Frame& frame) {
    if (!frame.dirty) return;
    // Write-ahead logging: the log must be on disk up to this page's LSN
    // *before* the page itself is written.
    if (flush_log_up_to_) {
        flush_log_up_to_(frame.page.lsn());
    }
    files_.at(frame.file_id)->write_page(frame.page_id, frame.page.bytes());
    frame.dirty = false;
}

void BufferPool::flush_page(int file_id, page_id_t page_id) {
    uint64_t key = make_key(file_id, page_id);
    auto it = page_table_.find(key);
    if (it == page_table_.end()) return;
    write_frame_to_disk(*it->second);
}

void BufferPool::flush_all() {
    for (auto& kv : page_table_) {
        write_frame_to_disk(*kv.second);
    }
}

void BufferPool::evict_one() {
    // Pick the least-recently-used *unpinned* frame (front of the LRU list).
    if (lru_.empty()) {
        throw BufferPoolFullException(
            "buffer pool is full and every page is pinned");
    }
    uint64_t victim_key = lru_.front();
    lru_.pop_front();
    lru_pos_.erase(victim_key);

    auto it = page_table_.find(victim_key);
    if (it == page_table_.end()) return;  // should not happen
    Frame* frame = it->second.get();
    write_frame_to_disk(*frame);  // flush if dirty (respects WAL)
    page_table_.erase(it);
}

}  // namespace minidb
