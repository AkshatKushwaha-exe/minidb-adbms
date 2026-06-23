// Baseline KVStore backed by the project's existing storage: a heap file for
// the records plus an in-memory B+ tree mapping key → RID. This is the
// "B+ Tree based storage" the LSM extension is compared against.
#pragma once

#include <memory>
#include <string>

#include "minidb/index/btree.h"
#include "minidb/lsm/kv_store.h"
#include "minidb/storage/buffer_pool.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/heap_file.h"

namespace minidb {

class BTreeStore : public KVStore {
public:
    explicit BTreeStore(const std::string& dir,
                        std::size_t buffer_pool_size = 256);
    ~BTreeStore() override;

    void put(const Value& key, const std::vector<uint8_t>& value) override;
    bool get(const Value& key, std::vector<uint8_t>& out) override;
    void remove(const Value& key) override;
    std::vector<std::pair<Value, std::vector<uint8_t>>> scan() override;
    void sync() override { bpool_->flush_all(); }  // write dirty pages to disk
    uint64_t disk_bytes() const override;
    std::string name() const override { return "B+Tree"; }

private:
    std::string dir_;
    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<BufferPool> bpool_;
    std::unique_ptr<HeapFile> heap_;
    std::unique_ptr<BTree> tree_;  // key -> RID (unique)
    int file_id_ = 0;
};

}  // namespace minidb
