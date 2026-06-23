#include "minidb/lsm/btree_store.h"

#include <filesystem>

namespace minidb {

BTreeStore::BTreeStore(const std::string& dir, std::size_t buffer_pool_size)
    : dir_(dir) {
    std::filesystem::create_directories(dir_);
    disk_ = std::make_unique<DiskManager>(dir_ + "/btree.db");
    bpool_ = std::make_unique<BufferPool>(buffer_pool_size);
    file_id_ = bpool_->register_file(disk_.get());
    heap_ = std::make_unique<HeapFile>(bpool_.get(), file_id_);
    tree_ = std::make_unique<BTree>();
    // Rebuild the index from any existing heap data (mirrors how the engine
    // rebuilds indexes on open). The value's key is recovered from storage by
    // the caller's convention; here we simply start fresh per benchmark run.
}

BTreeStore::~BTreeStore() {
    if (bpool_) bpool_->flush_all();
}

void BTreeStore::put(const Value& key, const std::vector<uint8_t>& value) {
    auto rids = tree_->search(key);
    if (!rids.empty()) {
        // Overwrite: remove the old record + index entry first.
        heap_->remove(rids[0]);
        tree_->erase(key, rids[0]);
    }
    RID rid = heap_->insert(value);
    tree_->insert(key, rid);
}

bool BTreeStore::get(const Value& key, std::vector<uint8_t>& out) {
    auto rids = tree_->search(key);
    if (rids.empty()) return false;
    return heap_->get(rids[0], out);
}

void BTreeStore::remove(const Value& key) {
    auto rids = tree_->search(key);
    if (rids.empty()) return;
    heap_->remove(rids[0]);
    tree_->erase(key, rids[0]);
}

std::vector<std::pair<Value, std::vector<uint8_t>>> BTreeStore::scan() {
    std::vector<std::pair<Value, std::vector<uint8_t>>> out;
    auto rows = tree_->range(std::nullopt, true, std::nullopt, true);
    for (const auto& kr : rows) {
        std::vector<uint8_t> bytes;
        if (heap_->get(kr.second, bytes)) out.emplace_back(kr.first, bytes);
    }
    return out;
}

uint64_t BTreeStore::disk_bytes() const {
    // Heap pages on disk. (The B+ tree index is in memory, like in the engine.)
    return static_cast<uint64_t>(bpool_->file_page_count(file_id_)) * PAGE_SIZE;
}

}  // namespace minidb
