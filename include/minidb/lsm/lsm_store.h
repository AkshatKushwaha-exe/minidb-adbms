// The LSM-tree key-value store: the extension's main deliverable.
//
// Writes go to the MemTable (and the WAL). When the MemTable grows past a
// threshold it is flushed to a new, immutable SSTable on disk. Reads check the
// MemTable first, then the SSTables newest→oldest (a newer entry, including a
// tombstone, shadows an older one). When SSTables accumulate, compaction merges
// them into one, keeping the newest value per key and dropping tombstones.
//
//   write path:  put/del ─► WAL ─► MemTable ──(full)──► flush ─► SSTable(newest)
//   read  path:  MemTable ─► SSTable_n ─► … ─► SSTable_0   (first hit wins)
//   maintenance: compaction merges all SSTables ─► one SSTable
//
// This trades read amplification (a lookup may touch several files) for very
// fast, sequential-only writes — the classic LSM trade-off.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "minidb/lsm/kv_store.h"
#include "minidb/lsm/lsm_wal.h"
#include "minidb/lsm/memtable.h"
#include "minidb/lsm/sstable.h"

namespace minidb {
namespace lsm {

class LSMStore : public KVStore {
public:
    // `dir` holds the WAL and SSTable files. `memtable_limit` is the MemTable
    // size (in bytes) that triggers a flush — small in tests to force flushes.
    explicit LSMStore(const std::string& dir,
                      std::size_t memtable_limit = 1u << 20);  // 1 MiB
    ~LSMStore() override;

    void put(const Value& key, const std::vector<uint8_t>& value) override;
    bool get(const Value& key, std::vector<uint8_t>& out) override;
    void remove(const Value& key) override;
    std::vector<std::pair<Value, std::vector<uint8_t>>> scan() override;
    void sync() override { flush(); }  // flush MemTable -> SSTable (durable)
    uint64_t disk_bytes() const override;
    std::string name() const override { return "LSM"; }

    // Flush the active MemTable to a new SSTable (no-op if empty).
    void flush();

    // Merge all SSTables into a single one (newest value wins; tombstones gone).
    void compact();

    // Introspection for tests / benchmarks.
    std::size_t num_sstables() const { return sstables_.size(); }

private:
    void open();
    void maybe_flush();

    std::string dir_;
    std::size_t memtable_limit_;
    MemTable mem_;
    std::unique_ptr<LsmWal> wal_;
    // SSTables ordered newest (front) → oldest (back).
    std::vector<std::unique_ptr<SSTable>> sstables_;
    int next_seq_ = 0;
};

}  // namespace lsm
}  // namespace minidb
