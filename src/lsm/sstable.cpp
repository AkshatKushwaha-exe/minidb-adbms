#include "minidb/lsm/sstable.h"

#include <algorithm>
#include <cstring>

#include "minidb/exceptions.h"
#include "minidb/lsm/codec.h"

namespace minidb {
namespace lsm {

void SSTable::write(const std::string& path,
                    const std::vector<SSTableEntry>& entries) {
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        throw StorageException("SSTable: cannot create " + path);
    for (const auto& e : entries) {
        char flag = e.tombstone ? 1 : 0;
        out.write(&flag, 1);
        write_value(out, e.key);
        uint32_t len = e.tombstone ? 0 : static_cast<uint32_t>(e.value.size());
        out.write(reinterpret_cast<const char*>(&len), 4);
        if (len > 0)
            out.write(reinterpret_cast<const char*>(e.value.data()), len);
    }
    out.flush();
}

std::unique_ptr<SSTable> SSTable::open(const std::string& path, int seq) {
    auto t = std::make_unique<SSTable>();
    t->path_ = path;
    t->seq_ = seq;
    t->in_.open(path, std::ios::in | std::ios::binary);
    if (!t->in_.is_open())
        throw StorageException("SSTable: cannot open " + path);

    // Scan the whole file once to build the key index + Bloom filter.
    std::ifstream scan(path, std::ios::in | std::ios::binary);
    std::vector<Value> keys;
    std::vector<uint64_t> offsets;
    while (true) {
        uint64_t pos = static_cast<uint64_t>(scan.tellg());
        int flag = scan.get();
        if (flag == EOF) break;
        Value key;
        if (!read_value(scan, key)) break;
        uint32_t len;
        if (!scan.read(reinterpret_cast<char*>(&len), 4)) break;
        scan.seekg(len, std::ios::cur);  // skip the value bytes
        keys.push_back(key);
        offsets.push_back(pos);
    }
    scan.clear();
    scan.seekg(0, std::ios::end);
    t->file_size_ = static_cast<uint64_t>(scan.tellg());

    t->keys_ = std::move(keys);
    t->offsets_ = std::move(offsets);
    t->bloom_ = std::make_unique<BloomFilter>(t->keys_.size() + 1);
    for (const auto& k : t->keys_) t->bloom_->add(k);
    return t;
}

Lookup SSTable::get(const Value& key, std::vector<uint8_t>& out) {
    if (!bloom_->maybe_contains(key)) return Lookup::Absent;  // skip the read
    // Binary search the sorted in-memory keys.
    auto it = std::lower_bound(keys_.begin(), keys_.end(), key);
    if (it == keys_.end() || !(*it == key)) return Lookup::Absent;
    std::size_t idx = static_cast<std::size_t>(it - keys_.begin());

    in_.clear();
    in_.seekg(static_cast<std::streamoff>(offsets_[idx]), std::ios::beg);
    int flag = in_.get();
    Value k;
    read_value(in_, k);  // skip over the stored key
    uint32_t len;
    in_.read(reinterpret_cast<char*>(&len), 4);
    if (flag == 1) return Lookup::Deleted;  // tombstone
    out.assign(len, 0);
    if (len > 0) in_.read(reinterpret_cast<char*>(out.data()), len);
    return Lookup::Found;
}

std::vector<SSTableEntry> SSTable::read_all() {
    std::vector<SSTableEntry> entries;
    std::ifstream scan(path_, std::ios::in | std::ios::binary);
    while (true) {
        int flag = scan.get();
        if (flag == EOF) break;
        SSTableEntry e;
        if (!read_value(scan, e.key)) break;
        uint32_t len;
        if (!scan.read(reinterpret_cast<char*>(&len), 4)) break;
        e.tombstone = (flag == 1);
        e.value.assign(len, 0);
        if (len > 0) scan.read(reinterpret_cast<char*>(e.value.data()), len);
        entries.push_back(std::move(e));
    }
    return entries;
}

}  // namespace lsm
}  // namespace minidb
