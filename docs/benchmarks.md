# Benchmark report

All numbers come from the harness in `src/bench_main.cpp`, run with:

```bash
make bench
```

The harness creates a fresh database in `build/bench_data`, loads **20,000 rows**
into a table `bench (id INT PRIMARY KEY, cat INT, payload TEXT)`, and measures
four things. Absolute timings depend on the machine; the *ratios* are the
interesting part and are stable across runs.

## Results (development machine)

```
MiniDB benchmark (N = 20000 rows)
------------------------------------------------
1. Insert 20000 rows: 48.8 ms (409,637 rows/sec)
2. 2000 point lookups:
     index scan (id = ?):   12.3 ms  (0.0062 ms/query)
     full scan (payload=?): 13242.9 ms (6.62 ms/query)
     speedup from index: 1076x
3. Buffer pool (repeated lookups): 5 hits, 0 misses (100% hit ratio)
4. Join small(500) x bench(20000):
     result rows: 500
     index nested-loop join: 1.47 ms
------------------------------------------------
```

## 1. Insert throughput

~400,000 rows/sec, inserting inside a single transaction. Each insert writes a
WAL record, appends to a heap page through the buffer pool, and updates the
primary-key B+ tree. Batching in one transaction matters: a `COMMIT` forces the
log to disk (an `fflush`), so committing per row would be dominated by that
flush. This is the same reason real databases encourage batching writes in a
transaction.

## 2. Index scan vs full table scan — the headline result

This is the clearest demonstration of why indexes exist and why the optimizer's
job matters.

- `SELECT ... WHERE id = ?` uses the **primary-key B+ tree**: the optimizer turns
  it into an `IndexScan` (a point lookup that touches one leaf path), so each
  query is ~0.006 ms.
- `SELECT ... WHERE payload = ?` filters on a **non-indexed** column, so the
  optimizer must use a `SeqScan` that reads all 20,000 rows and applies the
  filter — ~6.6 ms per query.

The index path is **~1000× faster**. You can see the optimizer making this
decision yourself:

```sql
EXPLAIN SELECT * FROM bench WHERE id = 42;       -- -> IndexScan(bench.bench_pk)
EXPLAIN SELECT * FROM bench WHERE payload = 'x'; -- -> SeqScan(bench)
```

## 3. Buffer-pool effectiveness

After the data is loaded, repeatedly looking up the same row hits the buffer pool
every time (100% hit ratio): the pages it touches are already resident, so no
disk reads occur. This is the cache doing its job. Lowering the pool size below
the working set (the `buffer_pool_size` argument to `Engine`) makes misses and
evictions appear — the LRU policy then decides what to keep.

## 4. Join algorithm

Joining a 500-row table to the 20,000-row table on its primary key completes in
~1.5 ms because the optimizer chooses an **index nested-loop join**: for each of
the 500 outer rows it probes the inner table's B+ tree (an O(log n) lookup)
instead of scanning all 20,000 inner rows. A plain nested-loop join would do
500 × 20,000 = 10 million comparisons; the index nested-loop does ~500 × log(20000)
≈ 7,000 index steps.

## How to reproduce and vary

- Change `N` in `src/bench_main.cpp` to see how each metric scales (insert and
  full-scan are linear in `N`; index lookup is logarithmic).
- Pass a smaller buffer pool to the `Engine` constructor to force eviction and
  watch the hit ratio in metric 3 drop.

## Takeaways

1. Indexes convert an O(N) scan into an O(log N) lookup — here a 1000× win.
2. The cost-based optimizer is what actually *chooses* to use the index; without
   it every query would be a full scan.
3. The buffer pool removes repeated disk I/O entirely for a hot working set.
4. Picking the right join algorithm changes a join from quadratic to near-linear.
