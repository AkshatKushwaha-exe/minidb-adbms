#include "minidb/lsm/lsm_store.h"

#include <algorithm>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

namespace minidb {
namespace lsm {

static std::string sst_path(const std::string& dir, int seq) {
    return dir + "/sst_" + std::to_string(seq) + ".dat";
}

LSMStore::LSMStore(const std::string& dir, std::size_t memtable_limit)
    : dir_(dir), memtable_limit_(memtable_limit) {
    fs::create_directories(dir_);
    open();
}

LSMStore::~LSMStore() {
    // Clean shutdown: persist whatever is still in the MemTable.
    flush();
}

void LSMStore::open() {
    // 1. Load existing SSTables, sorted newest (highest seq) first.
    std::vector<std::pair<int, std::string>> found;
    if (fs::exists(dir_)) {
        for (const auto& e : fs::directory_iterator(dir_)) {
            std::string name = e.path().filename().string();
            if (name.rfind("sst_", 0) == 0 &&
                name.size() > 8 /* sst_ + .dat */) {
                int seq = std::stoi(name.substr(4));
                found.push_back({seq, e.path().string()});
            }
        }
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& f : found) {
        sstables_.push_back(SSTable::open(f.second, f.first));
        next_seq_ = std::max(next_seq_, f.first + 1);
    }

    // 2. Replay the WAL into the MemTable (the unflushed tail of writes).
    std::string wp = dir_ + "/wal.log";
    for (const auto& r : LsmWal::read_all(wp)) {
        if (r.del)
            mem_.remove(r.key);
        else
            mem_.put(r.key, r.value);
    }
    wal_ = std::make_unique<LsmWal>(wp);
}

void LSMStore::put(const Value& key, const std::vector<uint8_t>& value) {
    wal_->append(false, key, value);
    mem_.put(key, value);
    maybe_flush();
}

void LSMStore::remove(const Value& key) {
    wal_->append(true, key, {});
    mem_.remove(key);
    maybe_flush();
}

bool LSMStore::get(const Value& key, std::vector<uint8_t>& out) {
    // MemTable first (it holds the newest writes).
    Lookup r = mem_.lookup(key, out);
    if (r == Lookup::Found) return true;
    if (r == Lookup::Deleted) return false;
    // Then SSTables, newest → oldest; first hit wins.
    for (auto& sst : sstables_) {
        r = sst->get(key, out);
        if (r == Lookup::Found) return true;
        if (r == Lookup::Deleted) return false;
    }
    return false;
}

void LSMStore::maybe_flush() {
    if (mem_.approx_bytes() >= memtable_limit_) flush();
}

void LSMStore::flush() {
    if (mem_.empty()) return;
    // The MemTable is already sorted by key, so this is one sequential write.
    std::vector<SSTableEntry> entries;
    entries.reserve(mem_.count());
    for (const auto& kv : mem_.entries()) {
        entries.push_back({kv.first, kv.second.tombstone, kv.second.value});
    }
    int seq = next_seq_++;
    std::string path = sst_path(dir_, seq);
    SSTable::write(path, entries);
    sstables_.insert(sstables_.begin(), SSTable::open(path, seq));  // newest
    mem_.clear();
    wal_->flush();
    wal_->truncate();  // MemTable is now durable as an SSTable
}

void LSMStore::compact() {
    if (sstables_.size() <= 1) return;
    // Merge newest→oldest, keeping the first (newest) entry seen per key.
    std::map<Value, SSTableEntry> merged;
    for (auto& sst : sstables_) {                 // newest first
        for (auto& e : sst->read_all()) {
            merged.emplace(e.key, e);             // ignored if key already seen
        }
    }
    // Build the output, dropping tombstones (no older version survives a full
    // compaction). std::map keeps keys sorted for us.
    std::vector<SSTableEntry> out;
    for (auto& kv : merged) {
        if (!kv.second.tombstone) out.push_back(kv.second);
    }

    int seq = next_seq_++;
    std::string new_path = sst_path(dir_, seq);
    SSTable::write(new_path, out);

    // Remove the old files (close them first by clearing the vector).
    std::vector<std::string> old_paths;
    for (auto& sst : sstables_) old_paths.push_back(sst->path());
    sstables_.clear();
    for (const auto& p : old_paths) fs::remove(p);

    sstables_.push_back(SSTable::open(new_path, seq));
}

std::vector<std::pair<Value, std::vector<uint8_t>>> LSMStore::scan() {
    // Merge MemTable + all SSTables, newest wins, skip tombstones.
    std::map<Value, std::pair<bool, std::vector<uint8_t>>> view;  // key -> (tomb,val)
    // Oldest first so newer overwrites; SSTables are newest-first, so reverse.
    for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
        for (auto& e : (*it)->read_all())
            view[e.key] = {e.tombstone, e.value};
    }
    for (const auto& kv : mem_.entries())  // MemTable is newest of all
        view[kv.first] = {kv.second.tombstone, kv.second.value};

    std::vector<std::pair<Value, std::vector<uint8_t>>> out;
    for (auto& kv : view) {
        if (!kv.second.first) out.emplace_back(kv.first, kv.second.second);
    }
    return out;
}

uint64_t LSMStore::disk_bytes() const {
    uint64_t total = 0;
    for (const auto& sst : sstables_) total += sst->file_size();
    std::string wp = dir_ + "/wal.log";
    if (fs::exists(wp)) total += fs::file_size(wp);
    return total;
}

}  // namespace lsm
}  // namespace minidb
