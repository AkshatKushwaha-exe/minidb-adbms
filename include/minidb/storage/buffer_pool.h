// The buffer pool is the in-memory cache of pages sitting between the rest of
// the engine and the disk managers. It has a fixed number of frames and evicts
// pages using a simple LRU policy when it runs out of room.
//
// Key ideas demonstrated here:
//   * pin counts  -- a page in use cannot be evicted (pin > 0).
//   * dirty bit   -- only modified pages are written back on eviction/flush.
//   * LRU         -- the least-recently-used *unpinned* page is evicted first.
//   * write-ahead -- before a dirty page is written to disk, the WAL is flushed
//                    up to that page's LSN (set via set_log_flush_callback).
//
// The pool can hold pages from several files at once. Each file (table heap or
// index) registers its DiskManager and gets back a small integer file id; the
// (file_id, page_id) pair identifies a frame.
#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "minidb/constants.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/page.h"

namespace minidb {

class BufferPool {
public:
    explicit BufferPool(std::size_t capacity = DEFAULT_BUFFER_POOL_SIZE);

    // Register a file and obtain an auto-assigned file id. The pool does not
    // own the manager. (Convenient for tests.)
    int register_file(DiskManager* dm);

    // Register a file under a caller-chosen id. The engine uses this so that a
    // table's file id equals its stable catalog id (see catalog.h), which is
    // what the WAL records and recovery relies on.
    void register_file_with_id(int file_id, DiskManager* dm);

    // Number of pages currently allocated in the given file.
    page_id_t file_page_count(int file_id) const;

    // Fetch (file_id, page_id) into a frame, pin it and return a pointer to the
    // in-memory Page. The caller MUST unpin_page when done.
    Page* fetch_page(int file_id, page_id_t page_id);

    // Allocate a brand new page in the file, pin it and return it. The new
    // page id is written to *out_page_id.
    Page* new_page(int file_id, page_id_t* out_page_id);

    // Release a pin. If is_dirty, the frame is marked dirty so it is written
    // back later. A frame is only evictable once its pin count reaches 0.
    void unpin_page(int file_id, page_id_t page_id, bool is_dirty);

    // Write a single page back to disk if it is dirty.
    void flush_page(int file_id, page_id_t page_id);

    // Write every dirty page back to disk (used on shutdown / checkpoint).
    void flush_all();

    // Hook so the recovery manager can enforce write-ahead logging: the pool
    // calls this with a page's LSN right before writing that page to disk.
    void set_log_flush_callback(std::function<void(lsn_t)> cb) {
        flush_log_up_to_ = std::move(cb);
    }

    // Statistics for benchmarking / demonstrating the cache works.
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    std::size_t capacity() const { return capacity_; }
    std::size_t size() const { return page_table_.size(); }

private:
    struct Frame {
        Page page;
        int file_id = -1;
        page_id_t page_id = INVALID_PAGE_ID;
        int pin_count = 0;
        bool dirty = false;
    };

    // A frame is identified by (file_id, page_id) packed into a 64-bit key.
    static uint64_t make_key(int file_id, page_id_t page_id) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(file_id)) << 32) |
               static_cast<uint32_t>(page_id);
    }

    // Evict the least-recently-used unpinned frame. Throws if all are pinned.
    void evict_one();
    void write_frame_to_disk(Frame& frame);

    std::size_t capacity_;
    std::vector<DiskManager*> files_;  // index = file id

    // file/page key -> frame. The frame is heap-allocated so pointers stay
    // valid even as the map rehashes.
    std::unordered_map<uint64_t, std::unique_ptr<Frame>> page_table_;

    // LRU list of *unpinned* frame keys, most-recently-used at the back.
    std::list<uint64_t> lru_;
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lru_pos_;

    std::function<void(lsn_t)> flush_log_up_to_;

    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
};

}  // namespace minidb
