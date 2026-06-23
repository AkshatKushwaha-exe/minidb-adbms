// Tests for the WAL and crash recovery.
//
// A "crash" is simulated by destroying the buffer pool WITHOUT flushing it, so
// every dirty (in-memory) page is lost -- exactly what happens when a process
// is killed. The WAL file and the (zero-extended) heap file remain on disk, and
// recovery must rebuild the committed state from the log.
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "minidb/recovery/recovery_manager.h"
#include "minidb/recovery/wal.h"
#include "minidb/storage/buffer_pool.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/heap_file.h"
#include "test_framework.h"
#include "test_util.h"

using namespace minidb;

static std::vector<uint8_t> B(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
static std::string S(const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
}
static std::set<std::string> scan_table(HeapFile& heap) {
    std::set<std::string> out;
    for (auto it = heap.begin(); it != heap.end(); ++it) {
        out.insert(S((*it).second));
    }
    return out;
}

TEST(wal, append_and_read_back) {
    std::string path = minitest::temp_path("wal1.wal");
    {
        WAL wal(path);
        wal.log_begin(1);
        wal.log_insert(1, 0, RID{0, 0}, B("hello"));
        wal.log_commit(1);
    }
    auto recs = WAL::read_all(path);
    CHECK_EQ(recs.size(), (size_t)3);
    CHECK(recs[0].type == LogType::BEGIN);
    CHECK(recs[1].type == LogType::INSERT);
    CHECK_EQ(S(recs[1].image), std::string("hello"));
    CHECK(recs[2].type == LogType::COMMIT);
    // LSNs are sequential.
    CHECK_EQ(recs[0].lsn, (lsn_t)0);
    CHECK_EQ(recs[2].lsn, (lsn_t)2);
}

TEST(wal, reopen_continues_lsn) {
    std::string path = minitest::temp_path("wal2.wal");
    { WAL wal(path); wal.log_begin(1); wal.log_commit(1); }
    { WAL wal(path); CHECK_EQ(wal.log_begin(2), (lsn_t)2); }
}

TEST(recovery, committed_survive_uncommitted_rolled_back) {
    std::string heap_path = minitest::temp_path("rec1.db");
    std::string wal_path = minitest::temp_path("rec1.wal");

    // --- run some work, then "crash" (no buffer flush) ---
    {
        DiskManager dm(heap_path);
        BufferPool bp(4);
        bp.register_file_with_id(0, &dm);
        WAL wal(wal_path);
        bp.set_log_flush_callback([&](lsn_t l) { wal.flush_to_lsn(l); });
        HeapFile heap(&bp, 0, &wal);

        // Committed transaction 1 inserts A and B.
        wal.log_begin(1);
        heap.insert(B("A"), 1);
        heap.insert(B("B"), 1);
        wal.log_commit(1);

        // Transaction 2 inserts C but never commits.
        wal.log_begin(2);
        heap.insert(B("C"), 2);
        // CRASH: scope ends; buffer pool is dropped without flush_all().
    }

    // --- recover and verify ---
    {
        DiskManager dm(heap_path);
        BufferPool bp(4);
        bp.register_file_with_id(0, &dm);
        RecoveryManager rec(&bp, wal_path);
        RecoveryStats stats = rec.recover();
        CHECK_EQ(stats.committed_txns, 1);
        CHECK_EQ(stats.loser_txns, 1);

        HeapFile heap(&bp, 0);
        auto rows = scan_table(heap);
        CHECK(rows.count("A") == 1);
        CHECK(rows.count("B") == 1);
        CHECK(rows.count("C") == 0);  // uncommitted insert rolled back
        CHECK_EQ(rows.size(), (size_t)2);
    }
}

TEST(recovery, explicit_abort_rolled_back) {
    std::string heap_path = minitest::temp_path("rec2.db");
    std::string wal_path = minitest::temp_path("rec2.wal");
    {
        DiskManager dm(heap_path);
        BufferPool bp(4);
        bp.register_file_with_id(0, &dm);
        WAL wal(wal_path);
        bp.set_log_flush_callback([&](lsn_t l) { wal.flush_to_lsn(l); });
        HeapFile heap(&bp, 0, &wal);

        wal.log_begin(1);
        heap.insert(B("keep"), 1);
        wal.log_commit(1);

        wal.log_begin(2);
        heap.insert(B("rollback_me"), 2);
        wal.log_abort(2);  // explicit abort
    }
    {
        DiskManager dm(heap_path);
        BufferPool bp(4);
        bp.register_file_with_id(0, &dm);
        RecoveryManager rec(&bp, wal_path);
        rec.recover();
        HeapFile heap(&bp, 0);
        auto rows = scan_table(heap);
        CHECK(rows.count("keep") == 1);
        CHECK(rows.count("rollback_me") == 0);
    }
}

TEST(recovery, committed_delete_is_durable) {
    std::string heap_path = minitest::temp_path("rec3.db");
    std::string wal_path = minitest::temp_path("rec3.wal");
    RID to_delete;
    {
        DiskManager dm(heap_path);
        BufferPool bp(4);
        bp.register_file_with_id(0, &dm);
        WAL wal(wal_path);
        bp.set_log_flush_callback([&](lsn_t l) { wal.flush_to_lsn(l); });
        HeapFile heap(&bp, 0, &wal);

        wal.log_begin(1);
        heap.insert(B("x"), 1);
        to_delete = heap.insert(B("y"), 1);
        heap.insert(B("z"), 1);
        wal.log_commit(1);
        bp.flush_all();  // pretend a checkpoint flushed everything

        // txn 2 deletes y and commits
        wal.log_begin(2);
        heap.remove(to_delete, 2);
        wal.log_commit(2);
        // crash before flushing the delete's page
    }
    {
        DiskManager dm(heap_path);
        BufferPool bp(4);
        bp.register_file_with_id(0, &dm);
        RecoveryManager rec(&bp, wal_path);
        rec.recover();
        HeapFile heap(&bp, 0);
        auto rows = scan_table(heap);
        CHECK(rows.count("x") == 1);
        CHECK(rows.count("z") == 1);
        CHECK(rows.count("y") == 0);  // committed delete survived the crash
    }
}

TEST(recovery, is_idempotent_when_run_twice) {
    std::string heap_path = minitest::temp_path("rec4.db");
    std::string wal_path = minitest::temp_path("rec4.wal");
    {
        DiskManager dm(heap_path);
        BufferPool bp(4);
        bp.register_file_with_id(0, &dm);
        WAL wal(wal_path);
        bp.set_log_flush_callback([&](lsn_t l) { wal.flush_to_lsn(l); });
        HeapFile heap(&bp, 0, &wal);
        wal.log_begin(1);
        heap.insert(B("one"), 1);
        heap.insert(B("two"), 1);
        wal.log_commit(1);
    }
    auto recover_and_scan = [&]() {
        DiskManager dm(heap_path);
        BufferPool bp(4);
        bp.register_file_with_id(0, &dm);
        RecoveryManager rec(&bp, wal_path);
        rec.recover();
        HeapFile heap(&bp, 0);
        return scan_table(heap);
    };
    auto first = recover_and_scan();
    auto second = recover_and_scan();  // running again must not duplicate
    CHECK(first == second);
    CHECK_EQ(second.size(), (size_t)2);
}
