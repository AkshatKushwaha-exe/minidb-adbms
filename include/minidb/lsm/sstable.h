// An SSTable (Sorted String Table): an immutable, sorted, on-disk file of
// key→value entries produced by flushing a MemTable (or by compaction).
//
// On disk it is just a sequence of entries sorted by key:
//   [flag:1 (0=value, 1=tombstone)] [key] [value-len:4] [value bytes]
//
// In memory we keep only a *sparse-ish* index — every key with the byte offset
// of its entry — plus a Bloom filter. A point lookup checks the Bloom filter,
// binary-searches the in-memory keys, then seeks to the offset and reads the
// value from disk. Values themselves stay on disk, which is what keeps an LSM's
// memory footprint small.
#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "minidb/lsm/bloom_filter.h"
#include "minidb/lsm/memtable.h"  // for Lookup
#include "minidb/record/value.h"

namespace minidb {
namespace lsm {

struct SSTableEntry {
    Value key;
    bool tombstone = false;
    std::vector<uint8_t> value;
};

class SSTable {
public:
    // Write `entries` (which MUST be sorted ascending by key) to `path`.
    static void write(const std::string& path,
                      const std::vector<SSTableEntry>& entries);

    // Open an existing SSTable file, building its in-memory index + Bloom filter.
    static std::unique_ptr<SSTable> open(const std::string& path, int seq);

    // Point lookup. Returns Found (out set), Deleted (tombstone), or Absent.
    Lookup get(const Value& key, std::vector<uint8_t>& out);

    // Read every entry in key order (used by compaction and full scans).
    std::vector<SSTableEntry> read_all();

    uint64_t file_size() const { return file_size_; }
    std::size_t num_keys() const { return keys_.size(); }
    int seq() const { return seq_; }
    const std::string& path() const { return path_; }

    SSTable(const SSTable&) = delete;
    SSTable& operator=(const SSTable&) = delete;
    SSTable() = default;

private:
    std::string path_;
    int seq_ = 0;
    uint64_t file_size_ = 0;
    std::vector<Value> keys_;       // sorted ascending
    std::vector<uint64_t> offsets_; // parallel: byte offset of each entry
    std::unique_ptr<BloomFilter> bloom_;
    std::ifstream in_;              // kept open for reads
};

}  // namespace lsm
}  // namespace minidb
