// Tests for the storage layer: Page, DiskManager, BufferPool, HeapFile.
#include <string>
#include <vector>

#include "minidb/storage/buffer_pool.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/heap_file.h"
#include "minidb/storage/page.h"
#include "test_framework.h"
#include "test_util.h"

using namespace minidb;

static std::vector<uint8_t> bytes_of(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
static std::string string_of(const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
}

TEST(page, insert_get_delete) {
    Page p;
    CHECK_EQ(p.num_slots(), 0);
    int s0 = p.insert_record(bytes_of("hello"));
    int s1 = p.insert_record(bytes_of("world!"));
    CHECK_EQ(s0, 0);
    CHECK_EQ(s1, 1);
    CHECK_EQ(p.num_slots(), 2);

    std::vector<uint8_t> out;
    CHECK(p.get_record(s0, out));
    CHECK_EQ(string_of(out), std::string("hello"));
    CHECK(p.get_record(s1, out));
    CHECK_EQ(string_of(out), std::string("world!"));

    CHECK(p.delete_record(s0));
    CHECK(!p.get_record(s0, out));    // tombstoned
    CHECK(!p.delete_record(s0));      // double delete fails
    CHECK(p.get_record(s1, out));     // sibling still there
    CHECK_EQ(string_of(out), std::string("world!"));
}

TEST(page, lsn_roundtrip) {
    Page p;
    p.set_lsn(123456789);
    CHECK_EQ(p.lsn(), (lsn_t)123456789);
}

TEST(page, full_page_returns_minus_one) {
    Page p;
    // Insert until it fills up; the last insert should fail with -1.
    std::vector<uint8_t> big(1000, 'x');
    int count = 0;
    while (true) {
        int s = p.insert_record(big);
        if (s < 0) break;
        ++count;
        if (count > 10) break;  // safety
    }
    CHECK(count >= 3);  // ~4080 bytes / (1000+4) => 4 records
    CHECK(count <= 4);
}

TEST(page, insert_at_for_recovery) {
    Page p;
    // Simulate recovery placing a record at slot 3 directly.
    CHECK(p.insert_record_at(3, bytes_of("r3")));
    CHECK_EQ(p.num_slots(), 4);
    std::vector<uint8_t> out;
    CHECK(p.get_record(3, out));
    CHECK_EQ(string_of(out), std::string("r3"));
    // The gap slots 0..2 are tombstones.
    CHECK(!p.get_record(0, out));
}

TEST(disk_manager, allocate_read_write) {
    std::string path = minitest::temp_path("dm.db");
    {
        DiskManager dm(path);
        CHECK_EQ(dm.num_pages(), 0);
        page_id_t p0 = dm.allocate_page();
        page_id_t p1 = dm.allocate_page();
        CHECK_EQ(p0, 0);
        CHECK_EQ(p1, 1);
        CHECK_EQ(dm.num_pages(), 2);

        std::vector<uint8_t> buf(PAGE_SIZE, 0);
        buf[0] = 0xAB;
        buf[PAGE_SIZE - 1] = 0xCD;
        dm.write_page(1, buf);
    }
    // Reopen: data and page count must persist.
    {
        DiskManager dm(path);
        CHECK_EQ(dm.num_pages(), 2);
        std::vector<uint8_t> buf;
        dm.read_page(1, buf);
        CHECK_EQ((int)buf[0], 0xAB);
        CHECK_EQ((int)buf[PAGE_SIZE - 1], 0xCD);
    }
}

TEST(buffer_pool, hit_miss_and_persistence) {
    std::string path = minitest::temp_path("bp.db");
    DiskManager dm(path);
    BufferPool bp(4);
    int fid = bp.register_file(&dm);

    page_id_t pid;
    Page* page = bp.new_page(fid, &pid);
    page->insert_record(bytes_of("persist me"));
    bp.unpin_page(fid, pid, /*dirty=*/true);

    // Fetch again: should be a hit (still resident).
    Page* again = bp.fetch_page(fid, pid);
    std::vector<uint8_t> out;
    CHECK(again->get_record(0, out));
    CHECK_EQ(string_of(out), std::string("persist me"));
    bp.unpin_page(fid, pid, false);
    CHECK(bp.hits() >= 1);

    bp.flush_all();
}

TEST(buffer_pool, lru_eviction) {
    std::string path = minitest::temp_path("lru.db");
    DiskManager dm(path);
    BufferPool bp(2);  // only 2 frames
    int fid = bp.register_file(&dm);

    // Create 3 pages, each with a distinct record, unpinning after each.
    std::vector<page_id_t> pids;
    for (int i = 0; i < 3; ++i) {
        page_id_t pid;
        Page* p = bp.new_page(fid, &pid);
        p->insert_record(bytes_of("page" + std::to_string(i)));
        bp.unpin_page(fid, pid, true);
        pids.push_back(pid);
    }
    // With capacity 2 and 3 pages, at least one eviction happened.
    CHECK(bp.size() <= 2);

    // All three must still be readable (evicted ones reload from disk).
    for (int i = 0; i < 3; ++i) {
        Page* p = bp.fetch_page(fid, pids[i]);
        std::vector<uint8_t> out;
        CHECK(p->get_record(0, out));
        CHECK_EQ(string_of(out), std::string("page" + std::to_string(i)));
        bp.unpin_page(fid, pids[i], false);
    }
}

TEST(buffer_pool, all_pinned_throws) {
    std::string path = minitest::temp_path("pin.db");
    DiskManager dm(path);
    BufferPool bp(2);
    int fid = bp.register_file(&dm);
    page_id_t a, b;
    bp.new_page(fid, &a);  // pinned
    bp.new_page(fid, &b);  // pinned
    // Third page needs a frame but both are pinned -> exception.
    CHECK_THROWS(bp.new_page(fid, nullptr));
}

TEST(heap_file, insert_get_remove_scan) {
    std::string path = minitest::temp_path("heap.db");
    DiskManager dm(path);
    BufferPool bp(8);
    int fid = bp.register_file(&dm);
    HeapFile heap(&bp, fid);

    RID r1 = heap.insert(bytes_of("alice"));
    RID r2 = heap.insert(bytes_of("bob"));
    RID r3 = heap.insert(bytes_of("carol"));

    std::vector<uint8_t> out;
    CHECK(heap.get(r2, out));
    CHECK_EQ(string_of(out), std::string("bob"));

    CHECK(heap.remove(r2));
    CHECK(!heap.get(r2, out));

    // Scan should now see alice and carol only.
    int seen = 0;
    bool saw_alice = false, saw_carol = false, saw_bob = false;
    for (auto it = heap.begin(); it != heap.end(); ++it) {
        auto pr = *it;
        std::string s = string_of(pr.second);
        if (s == "alice") saw_alice = true;
        if (s == "bob") saw_bob = true;
        if (s == "carol") saw_carol = true;
        ++seen;
    }
    CHECK_EQ(seen, 2);
    CHECK(saw_alice);
    CHECK(saw_carol);
    CHECK(!saw_bob);
    (void)r1;
    (void)r3;
}

TEST(heap_file, spans_multiple_pages) {
    std::string path = minitest::temp_path("heap_big.db");
    DiskManager dm(path);
    BufferPool bp(4);
    int fid = bp.register_file(&dm);
    HeapFile heap(&bp, fid);

    // Insert enough ~500 byte records to force several pages.
    const int N = 50;
    std::vector<uint8_t> rec(500, 'z');
    for (int i = 0; i < N; ++i) {
        // make each record distinguishable by first 4 bytes
        rec[0] = (uint8_t)(i & 0xFF);
        heap.insert(rec);
    }
    CHECK(heap.page_count() >= 2);

    int seen = 0;
    for (auto it = heap.begin(); it != heap.end(); ++it) {
        ++seen;
    }
    CHECK_EQ(seen, N);
}
