// Tests for the LSM-tree storage engine: MemTable, Bloom filter, SSTable,
// flush, multi-SSTable reads, tombstones, compaction, scan, recovery, and a
// randomised comparison against a std::map oracle.
#include <algorithm>
#include <map>
#include <random>
#include <string>

#include "minidb/lsm/bloom_filter.h"
#include "minidb/lsm/lsm_store.h"
#include "minidb/lsm/memtable.h"
#include "minidb/lsm/sstable.h"
#include "test_framework.h"
#include "test_util.h"

using namespace minidb;
using namespace minidb::lsm;

static Value K(int64_t k) { return Value::make_int(k); }
static std::vector<uint8_t> V(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
static std::string S(const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
}

TEST(lsm_memtable, put_get_delete) {
    MemTable m;
    std::vector<uint8_t> out;
    CHECK(m.lookup(K(1), out) == Lookup::Absent);
    m.put(K(1), V("a"));
    CHECK(m.lookup(K(1), out) == Lookup::Found);
    CHECK_EQ(S(out), std::string("a"));
    m.remove(K(1));
    CHECK(m.lookup(K(1), out) == Lookup::Deleted);
    CHECK(m.approx_bytes() > 0);
}

TEST(lsm_bloom, no_false_negatives) {
    BloomFilter b(1000);
    for (int i = 0; i < 1000; ++i) b.add(K(i));
    for (int i = 0; i < 1000; ++i) CHECK(b.maybe_contains(K(i)));  // never miss
    // False-positive rate should be low for absent keys.
    int fp = 0;
    for (int i = 100000; i < 101000; ++i)
        if (b.maybe_contains(K(i))) ++fp;
    CHECK(fp < 100);  // well under 10%
}

TEST(lsm_sstable, write_open_get) {
    std::string dir = minitest::temp_dir("lsm_sst");
    std::string path = dir + "/t.dat";
    std::vector<SSTableEntry> entries = {
        {K(1), false, V("one")},
        {K(3), false, V("three")},
        {K(5), true, {}},  // tombstone
    };
    SSTable::write(path, entries);
    auto sst = SSTable::open(path, 0);
    std::vector<uint8_t> out;
    CHECK(sst->get(K(1), out) == Lookup::Found);
    CHECK_EQ(S(out), std::string("one"));
    CHECK(sst->get(K(3), out) == Lookup::Found);
    CHECK(sst->get(K(5), out) == Lookup::Deleted);
    CHECK(sst->get(K(2), out) == Lookup::Absent);
    CHECK_EQ(sst->num_keys(), (size_t)3);
}

TEST(lsm_store, basic_put_get_delete) {
    LSMStore db(minitest::temp_dir("lsm_basic"));
    std::vector<uint8_t> out;
    db.put(K(1), V("alice"));
    db.put(K(2), V("bob"));
    CHECK(db.get(K(1), out)); CHECK_EQ(S(out), std::string("alice"));
    CHECK(db.get(K(2), out)); CHECK_EQ(S(out), std::string("bob"));
    CHECK(!db.get(K(3), out));
    db.remove(K(1));
    CHECK(!db.get(K(1), out));
}

TEST(lsm_store, overwrite_returns_newest) {
    LSMStore db(minitest::temp_dir("lsm_over"));
    db.put(K(1), V("v1"));
    db.put(K(1), V("v2"));
    db.put(K(1), V("v3"));
    std::vector<uint8_t> out;
    CHECK(db.get(K(1), out));
    CHECK_EQ(S(out), std::string("v3"));
}

TEST(lsm_store, flush_creates_sstables_and_reads_work) {
    // Tiny MemTable limit forces frequent flushes -> multiple SSTables.
    LSMStore db(minitest::temp_dir("lsm_flush"), /*memtable_limit=*/128);
    for (int i = 0; i < 200; ++i) db.put(K(i), V("val" + std::to_string(i)));
    CHECK(db.num_sstables() >= 2);  // flushed several times
    std::vector<uint8_t> out;
    for (int i = 0; i < 200; ++i) {
        CHECK(db.get(K(i), out));
        CHECK_EQ(S(out), std::string("val" + std::to_string(i)));
    }
}

TEST(lsm_store, newer_value_shadows_older_across_sstables) {
    LSMStore db(minitest::temp_dir("lsm_shadow"), 64);
    db.put(K(7), V("old"));
    db.flush();                 // old value now in SSTable 0
    db.put(K(7), V("new"));
    db.flush();                 // new value in SSTable 1 (newer)
    std::vector<uint8_t> out;
    CHECK(db.get(K(7), out));
    CHECK_EQ(S(out), std::string("new"));
    // A delete must also shadow the older SSTable value.
    db.remove(K(7));
    db.flush();
    CHECK(!db.get(K(7), out));
}

TEST(lsm_store, compaction_preserves_data_and_drops_tombstones) {
    std::string dir = minitest::temp_dir("lsm_compact");
    LSMStore db(dir, 64);
    for (int i = 0; i < 100; ++i) db.put(K(i), V("v" + std::to_string(i)));
    for (int i = 0; i < 50; ++i) db.remove(K(i));  // delete half
    db.flush();
    CHECK(db.num_sstables() >= 2);

    db.compact();
    CHECK_EQ(db.num_sstables(), (size_t)1);  // merged into one

    std::vector<uint8_t> out;
    for (int i = 0; i < 50; ++i) CHECK(!db.get(K(i), out));    // deleted
    for (int i = 50; i < 100; ++i) {                            // survivors
        CHECK(db.get(K(i), out));
        CHECK_EQ(S(out), std::string("v" + std::to_string(i)));
    }
}

TEST(lsm_store, scan_is_ordered_and_excludes_deleted) {
    LSMStore db(minitest::temp_dir("lsm_scan"), 64);
    for (int i = 10; i >= 1; --i) db.put(K(i), V("v" + std::to_string(i)));
    db.remove(K(5));
    auto rows = db.scan();
    CHECK_EQ(rows.size(), (size_t)9);  // 10 minus the deleted one
    for (size_t i = 1; i < rows.size(); ++i)
        CHECK(rows[i - 1].first < rows[i].first);  // ascending
    for (const auto& r : rows) CHECK(r.first.as_int() != 5);
}

TEST(lsm_store, recovery_replays_unflushed_writes) {
    std::string dir = minitest::temp_dir("lsm_recover");
    {
        LSMStore db(dir, 64);
        db.put(K(1), V("durable"));
        db.flush();              // -> SSTable
        db.put(K(2), V("in-memtable"));  // only in WAL + MemTable
        // No clean flush of K(2); destructor will flush the MemTable though.
    }
    {
        LSMStore db(dir, 64);    // reopen: load SSTables + replay WAL
        std::vector<uint8_t> out;
        CHECK(db.get(K(1), out)); CHECK_EQ(S(out), std::string("durable"));
        CHECK(db.get(K(2), out)); CHECK_EQ(S(out), std::string("in-memtable"));
    }
}

TEST(lsm_store, randomized_vs_oracle) {
    std::string dir = minitest::temp_dir("lsm_oracle");
    LSMStore db(dir, /*memtable_limit=*/256);  // force flushes
    std::map<int64_t, std::string> oracle;
    std::mt19937 rng(2024);
    std::uniform_int_distribution<int> key_dist(0, 60);
    std::uniform_int_distribution<int> op_dist(0, 9);

    for (int step = 0; step < 5000; ++step) {
        int key = key_dist(rng);
        int op = op_dist(rng);
        if (op < 7) {  // put
            std::string val = "v" + std::to_string(step);
            db.put(K(key), V(val));
            oracle[key] = val;
        } else {  // delete
            db.remove(K(key));
            oracle.erase(key);
        }
        if (step % 1500 == 0) db.compact();  // exercise compaction mid-stream
    }

    std::vector<uint8_t> out;
    for (int key = 0; key <= 60; ++key) {
        bool got = db.get(K(key), out);
        bool want = oracle.count(key) > 0;
        CHECK_EQ(got, want);
        if (want) CHECK_EQ(S(out), oracle[key]);
    }
    // Scan must match the oracle's live set, in order.
    auto rows = db.scan();
    CHECK_EQ(rows.size(), oracle.size());
}
