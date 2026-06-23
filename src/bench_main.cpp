// Benchmark harness for MiniDB.
//
// Measures a few things the report cares about:
//   1. Insert throughput (rows/sec).
//   2. Point-lookup latency: index scan (PK equality) vs full table scan.
//   3. Buffer-pool effectiveness (hit ratio under a repeated-scan workload).
//   4. Join latency: index nested-loop vs plain nested-loop.
//
// Run with: make bench
#include <chrono>
#include <iostream>
#include <string>

#include "minidb/engine.h"

using namespace minidb;
using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main() {
    const std::string dir = "build/bench_data";
    std::system(("rm -rf " + dir).c_str());

    const int N = 20000;
    std::cout << "MiniDB benchmark (N = " << N << " rows)\n";
    std::cout << "------------------------------------------------\n";

    Engine db(dir, /*buffer_pool_size=*/256);
    db.execute("CREATE TABLE bench (id INT PRIMARY KEY, cat INT, payload TEXT)");

    // --- 1. Insert throughput (batched inside one transaction) -------------
    auto t0 = Clock::now();
    db.execute("BEGIN");
    for (int i = 0; i < N; ++i) {
        db.execute("INSERT INTO bench VALUES (" + std::to_string(i) + ", " +
                   std::to_string(i % 100) + ", 'row" + std::to_string(i) + "')");
    }
    db.execute("COMMIT");
    double insert_ms = ms_since(t0);
    std::cout << "1. Insert " << N << " rows: " << insert_ms << " ms ("
              << (N / (insert_ms / 1000.0)) << " rows/sec)\n";

    // --- 2. Point lookup: index scan vs full scan --------------------------
    const int probes = 2000;
    t0 = Clock::now();
    for (int i = 0; i < probes; ++i) {
        int key = (i * 7919) % N;  // pseudo-random keys
        db.execute("SELECT payload FROM bench WHERE id = " + std::to_string(key));
    }
    double index_ms = ms_since(t0);

    t0 = Clock::now();
    for (int i = 0; i < probes; ++i) {
        int key = (i * 7919) % N;
        // payload is not indexed -> forces a full table scan + filter
        db.execute("SELECT id FROM bench WHERE payload = 'row" +
                   std::to_string(key) + "'");
    }
    double scan_ms = ms_since(t0);

    std::cout << "2. " << probes << " point lookups:\n";
    std::cout << "     index scan (id = ?):   " << index_ms << " ms ("
              << (index_ms / probes) << " ms/query)\n";
    std::cout << "     full scan (payload=?): " << scan_ms << " ms ("
              << (scan_ms / probes) << " ms/query)\n";
    std::cout << "     speedup from index: " << (scan_ms / index_ms) << "x\n";

    // --- 3. Buffer-pool hit ratio ------------------------------------------
    {
        uint64_t h0 = db.buffer_pool().hits();
        uint64_t m0 = db.buffer_pool().misses();
        for (int r = 0; r < 5; ++r) db.execute("SELECT id FROM bench WHERE id = 1");
        uint64_t h = db.buffer_pool().hits() - h0;
        uint64_t m = db.buffer_pool().misses() - m0;
        double ratio = (h + m) ? (100.0 * h / (h + m)) : 0.0;
        std::cout << "3. Buffer pool (repeated lookups): " << h << " hits, " << m
                  << " misses (" << ratio << "% hit ratio)\n";
    }

    // --- 4. Join: index nested-loop vs nested-loop -------------------------
    db.execute("CREATE TABLE small (sid INT PRIMARY KEY, ref INT)");
    db.execute("BEGIN");
    for (int i = 0; i < 500; ++i)
        db.execute("INSERT INTO small VALUES (" + std::to_string(i) + ", " +
                   std::to_string((i * 37) % N) + ")");
    db.execute("COMMIT");

    // small.ref = bench.id : bench has a PK index on id -> index nested loop.
    t0 = Clock::now();
    auto r1 = db.execute(
        "SELECT s.sid, b.payload FROM small s JOIN bench b ON s.ref = b.id");
    double inlj_ms = ms_since(t0);
    std::cout << "4. Join small(500) x bench(" << N << "):\n";
    std::cout << "     result rows: " << r1.select.rows.size() << "\n";
    std::cout << "     index nested-loop join: " << inlj_ms << " ms\n";

    std::cout << "------------------------------------------------\n";
    std::cout << "Done. (data in " << dir << ")\n";
    return 0;
}
