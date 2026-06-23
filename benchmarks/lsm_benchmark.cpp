// Extension benchmark: LSM-tree storage vs B+ tree/heap storage.
//
// Both engines implement the same KVStore interface, so we run the identical
// workload through each and compare:
//   * write throughput   (bulk load + random updates)
//   * read latency       (point lookups: hits and misses)
//   * space amplification (bytes on disk, and the effect of LSM compaction)
//
// Run with: make lsm-bench
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "minidb/lsm/btree_store.h"
#include "minidb/lsm/lsm_store.h"

using namespace minidb;
using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
static Value K(int64_t k) { return Value::make_int(k); }

struct Result {
    double load_ms = 0, update_ms = 0, read_hit_ms = 0, read_miss_ms = 0;
    uint64_t disk_after_load = 0;
};

// Run the same workload against a store. Seeds are fixed so both engines see the
// identical sequence of operations.
static Result run_workload(KVStore& store, int N, int updates, int reads) {
    Result r;
    std::vector<uint8_t> value(100, 'x');  // 100-byte payload

    // 1. Bulk load N distinct keys, then make them durable on disk.
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) store.put(K(i), value);
    store.sync();  // include the cost of getting data onto disk
    r.load_ms = ms_since(t0);
    r.disk_after_load = store.disk_bytes();

    // 2. Random in-place updates (overwrite existing keys) + durability.
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> kd(0, N - 1);
    t0 = Clock::now();
    for (int i = 0; i < updates; ++i) store.put(K(kd(rng)), value);
    store.sync();
    r.update_ms = ms_since(t0);

    // 3. Point reads that hit.
    std::vector<uint8_t> out;
    std::mt19937 rng2(7);
    t0 = Clock::now();
    for (int i = 0; i < reads; ++i) store.get(K(kd(rng2)), out);
    r.read_hit_ms = ms_since(t0);

    // 4. Point reads that miss (absent keys) -- highlights the Bloom filter.
    std::uniform_int_distribution<int> miss(N + 1, 2 * N);
    std::mt19937 rng3(9);
    t0 = Clock::now();
    for (int i = 0; i < reads; ++i) store.get(K(miss(rng3)), out);
    r.read_miss_ms = ms_since(t0);

    return r;
}

static void print_row(const std::string& label, double lsm, double bt,
                      const std::string& unit) {
    std::cout << "  " << label;
    for (std::size_t i = label.size(); i < 26; ++i) std::cout << ' ';
    std::cout << "LSM=" << lsm << unit << "   B+Tree=" << bt << unit;
    if (lsm > 0 && bt > 0) {
        if (lsm < bt) std::cout << "   (LSM " << (bt / lsm) << "x faster)";
        else std::cout << "   (B+Tree " << (lsm / bt) << "x faster)";
    }
    std::cout << "\n";
}

int main() {
    const int N = 50000;
    const int UPDATES = 50000;
    const int READS = 20000;

    std::cout << "LSM vs B+Tree storage benchmark\n";
    std::cout << "N=" << N << " keys, " << UPDATES << " random updates, " << READS
              << " reads (100-byte values)\n";
    std::cout << "------------------------------------------------------------\n";

    std::system("rm -rf build/lsm_bench_data");
    Result lsm_r, bt_r;
    uint64_t lsm_disk_precompact = 0, lsm_disk_postcompact = 0;
    std::size_t sst_before = 0, sst_after = 0;
    double lsm_hit_postcompact = 0, lsm_miss_postcompact = 0;
    {
        // Small MemTable so writes spill into many SSTables -> visible space &
        // read amplification, which compaction then reclaims.
        lsm::LSMStore lsm("build/lsm_bench_data/lsm", /*memtable_limit=*/256u << 10);
        lsm_r = run_workload(lsm, N, UPDATES, READS);
        sst_before = lsm.num_sstables();
        lsm_disk_precompact = lsm.disk_bytes();  // after load + updates

        // Compaction merges all SSTables into one: reclaim space + cut read
        // amplification.
        lsm.compact();
        sst_after = lsm.num_sstables();
        lsm_disk_postcompact = lsm.disk_bytes();

        std::vector<uint8_t> out;
        std::mt19937 rng(7);
        std::uniform_int_distribution<int> kd(0, N - 1);
        std::uniform_int_distribution<int> miss(N + 1, 2 * N);
        auto t0 = Clock::now();
        for (int i = 0; i < READS; ++i) lsm.get(K(kd(rng)), out);
        lsm_hit_postcompact = ms_since(t0);
        std::mt19937 rng3(9);
        t0 = Clock::now();
        for (int i = 0; i < READS; ++i) lsm.get(K(miss(rng3)), out);
        lsm_miss_postcompact = ms_since(t0);
    }
    {
        BTreeStore bt("build/lsm_bench_data/btree", /*buffer_pool=*/256);
        bt_r = run_workload(bt, N, UPDATES, READS);
    }

    std::cout << "\n== Write throughput ==\n";
    print_row("bulk load " + std::to_string(N), lsm_r.load_ms, bt_r.load_ms, "ms");
    print_row(std::to_string(UPDATES) + " updates", lsm_r.update_ms,
              bt_r.update_ms, "ms");
    std::cout << "    LSM load:    "
              << (N / (lsm_r.load_ms / 1000.0)) << " rows/sec\n";
    std::cout << "    B+Tree load: "
              << (N / (bt_r.load_ms / 1000.0)) << " rows/sec\n";

    std::cout << "\n== Read latency (" << READS << " point lookups) ==\n";
    print_row("hits  (" + std::to_string(sst_before) + " SSTables)",
              lsm_r.read_hit_ms, bt_r.read_hit_ms, "ms");
    print_row("misses (" + std::to_string(sst_before) + " SSTables)",
              lsm_r.read_miss_ms, bt_r.read_miss_ms, "ms");
    std::cout << "  LSM after compaction (1 SSTable):  hits=" << lsm_hit_postcompact
              << "ms   misses=" << lsm_miss_postcompact << "ms"
              << "   (read amplification cut by compaction)\n";

    std::cout << "\n== Space amplification ==\n";
    double logical = static_cast<double>(N) * (8 + 100);
    std::cout << "  logical data size ~ " << (uint64_t)logical << " bytes ("
              << N << " live keys)\n";
    std::cout << "  LSM pre-compaction:  " << lsm_disk_precompact << " bytes, "
              << sst_before << " SSTables  ("
              << (lsm_disk_precompact / logical) << "x)\n";
    std::cout << "  LSM post-compaction: " << lsm_disk_postcompact << " bytes, "
              << sst_after << " SSTable   (" << (lsm_disk_postcompact / logical)
              << "x)\n";
    std::cout << "  B+Tree (heap):       " << bt_r.disk_after_load << " bytes  ("
              << (bt_r.disk_after_load / logical) << "x)\n";

    std::cout << "------------------------------------------------------------\n";
    std::cout << "Done.\n";
    return 0;
}
