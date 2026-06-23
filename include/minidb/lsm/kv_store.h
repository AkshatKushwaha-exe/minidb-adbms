// A minimal key→value storage interface, so the LSM engine and the existing
// B+ tree/heap engine can be benchmarked through the exact same API.
//
// Keys are `Value`s; values are opaque byte blobs (in the benchmark, a
// serialised row). This is the contract the extension's comparison is built on.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "minidb/record/value.h"

namespace minidb {

class KVStore {
public:
    virtual ~KVStore() = default;

    // Insert or overwrite a key.
    virtual void put(const Value& key, const std::vector<uint8_t>& value) = 0;

    // Fetch a key; returns false if absent or deleted.
    virtual bool get(const Value& key, std::vector<uint8_t>& out) = 0;

    // Delete a key (no-op if absent).
    virtual void remove(const Value& key) = 0;

    // All live key→value pairs in ascending key order.
    virtual std::vector<std::pair<Value, std::vector<uint8_t>>> scan() = 0;

    // Force all buffered writes to disk (so write throughput is measured fairly,
    // i.e. including the cost of durability, not just the in-memory work).
    virtual void sync() = 0;

    // Total bytes this store occupies on disk (for space-amplification analysis).
    virtual uint64_t disk_bytes() const = 0;

    // Human-readable engine name (e.g. "LSM", "B+Tree").
    virtual std::string name() const = 0;
};

}  // namespace minidb
