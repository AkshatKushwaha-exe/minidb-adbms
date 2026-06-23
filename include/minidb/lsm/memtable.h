// The MemTable: the in-memory, write-side of the LSM tree.
//
// All writes (put/delete) land here first. It is an ordered map so that, when it
// fills up, it can be flushed to disk as a sorted SSTable in one sequential
// pass. A delete is recorded as a *tombstone* (a marker) rather than removing
// the key, because an older SSTable may still hold the key — the tombstone is
// what shadows it until compaction removes both.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "minidb/record/value.h"

namespace minidb {
namespace lsm {

enum class Lookup { Absent, Found, Deleted };

struct MemEntry {
    bool tombstone = false;
    std::vector<uint8_t> value;
};

class MemTable {
public:
    void put(const Value& key, const std::vector<uint8_t>& value) {
        adjust(key, value.size(), /*tombstone=*/false);
        map_[key] = MemEntry{false, value};
    }
    void remove(const Value& key) {
        adjust(key, 0, /*tombstone=*/true);
        map_[key] = MemEntry{true, {}};
    }

    Lookup lookup(const Value& key, std::vector<uint8_t>& out) const {
        auto it = map_.find(key);
        if (it == map_.end()) return Lookup::Absent;
        if (it->second.tombstone) return Lookup::Deleted;
        out = it->second.value;
        return Lookup::Found;
    }

    const std::map<Value, MemEntry>& entries() const { return map_; }
    std::size_t approx_bytes() const { return bytes_; }
    std::size_t count() const { return map_.size(); }
    bool empty() const { return map_.empty(); }
    void clear() {
        map_.clear();
        bytes_ = 0;
    }

private:
    // Rough byte accounting so we know when to flush. ~24 bytes/key overhead.
    void adjust(const Value& key, std::size_t new_val_size, bool tombstone) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            bytes_ -= it->second.value.size();
        } else {
            std::size_t key_size =
                key.type() == Type::INT ? 8 : key.as_text().size();
            bytes_ += key_size + 24;
        }
        bytes_ += tombstone ? 0 : new_val_size;
    }

    std::map<Value, MemEntry> map_;
    std::size_t bytes_ = 0;
};

}  // namespace lsm
}  // namespace minidb
