// Write-ahead log for the LSM MemTable.
//
// Every put/delete is appended here before (well, together with) updating the
// in-memory MemTable, so an unflushed MemTable can be rebuilt after a restart.
// Once the MemTable is flushed to an SSTable it is durable on its own, so we
// truncate the log at that point (the log only ever covers the *current*
// MemTable).
//
// Like the main engine's WAL, writes are OS-buffered rather than fsync'd per
// record: this keeps write throughput high (the whole point of an LSM tree) and
// still survives a process crash, which is the failure model we test.
#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "minidb/lsm/codec.h"
#include "minidb/record/value.h"

namespace minidb {
namespace lsm {

struct WalRecord {
    bool del;
    Value key;
    std::vector<uint8_t> value;
};

class LsmWal {
public:
    explicit LsmWal(const std::string& path) : path_(path) {
        out_.open(path_, std::ios::out | std::ios::app | std::ios::binary);
    }
    ~LsmWal() {
        if (out_.is_open()) out_.flush();
    }

    void append(bool del, const Value& key, const std::vector<uint8_t>& value) {
        std::vector<uint8_t> rec;
        rec.push_back(del ? 1 : 0);
        encode_value(key, rec);
        uint32_t len = del ? 0 : static_cast<uint32_t>(value.size());
        uint8_t b[4];
        std::memcpy(b, &len, 4);
        rec.insert(rec.end(), b, b + 4);
        if (!del) rec.insert(rec.end(), value.begin(), value.end());

        uint32_t total = static_cast<uint32_t>(rec.size());
        out_.write(reinterpret_cast<const char*>(&total), 4);
        out_.write(reinterpret_cast<const char*>(rec.data()), rec.size());
    }

    void flush() {
        if (out_.is_open()) out_.flush();
    }

    // Discard the log (called after the MemTable is safely flushed to disk).
    void truncate() {
        out_.close();
        out_.open(path_, std::ios::out | std::ios::trunc | std::ios::binary);
    }

    static std::vector<WalRecord> read_all(const std::string& path) {
        std::vector<WalRecord> out;
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in.is_open()) return out;
        while (true) {
            uint32_t total = 0;
            in.read(reinterpret_cast<char*>(&total), 4);
            if (in.gcount() < 4) break;
            std::vector<uint8_t> rec(total);
            in.read(reinterpret_cast<char*>(rec.data()), total);
            if (static_cast<uint32_t>(in.gcount()) < total) break;  // torn tail
            std::size_t pos = 0;
            WalRecord r;
            r.del = rec[pos++] != 0;
            r.key = decode_value(rec, pos);
            uint32_t len;
            std::memcpy(&len, rec.data() + pos, 4);
            pos += 4;
            r.value.assign(rec.begin() + pos, rec.begin() + pos + len);
            out.push_back(std::move(r));
        }
        return out;
    }

private:
    std::string path_;
    std::ofstream out_;
};

}  // namespace lsm
}  // namespace minidb
