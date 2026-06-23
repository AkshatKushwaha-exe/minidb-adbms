# Extension Track C — LSM-tree storage

## Motivation

The base engine stores each table as a **heap file** with an in-memory **B+ tree**
index. That design updates data *in place* and is excellent for point lookups,
but it is read-optimised: every write eventually becomes a (potentially random)
page write.

A **Log-Structured Merge tree (LSM-tree)** takes the opposite stance. It never
updates in place. Writes are absorbed by an in-memory table and then flushed to
disk as immutable, sorted files; obsolete data is cleaned up later in the
background. This turns all writes into large *sequential* I/O, which is the
design used by modern write-heavy stores (RocksDB, LevelDB, Cassandra).

This extension implements an LSM-tree storage engine and compares it against the
project's existing B+ tree/heap storage on the same workload.

## Design

```
   put / delete
        │  (append, sequential)
        ▼
   ┌──────────┐      flush when full       ┌───────────────────────┐
   │   WAL    │ ───────────────────────►   │  SSTable_n (newest)    │
   └────┬─────┘                            │  SSTable_n-1           │
        │                                  │  …                     │
        ▼                                  │  SSTable_0 (oldest)    │
   ┌──────────┐   flush (sorted, 1 pass)   └───────────┬───────────┘
   │ MemTable │ ──────────────────────────────────────┘
   │ (std::map)│            compaction merges all ► one SSTable
   └──────────┘

   get(key):  MemTable ─► SSTable_n ─► … ─► SSTable_0   (first hit wins)
```

### Components (`include/minidb/lsm/`, `src/lsm/`)

- **MemTable** (`memtable.h`) — an in-memory `std::map<Value, entry>` holding the
  newest writes. A delete inserts a **tombstone** (a marker) rather than erasing
  the key, because older SSTables may still contain it. It tracks an approximate
  byte size so we know when to flush.

- **Write-ahead log** (`lsm_wal.h`) — every put/delete is appended here before
  updating the MemTable, so an unflushed MemTable survives a crash. It is
  truncated once the MemTable is safely flushed (it only ever covers the current
  MemTable). Writes are OS-buffered, not fsync'd per op, matching the base
  engine's durability model (survives a process crash).

- **SSTable** (`sstable.h`, `sstable.cpp`) — an immutable, sorted on-disk file
  produced by flushing a MemTable. On disk it is just sorted
  `[flag][key][len][value]` entries. In memory we keep a **key→offset index**
  and a **Bloom filter**; values stay on disk and are read on demand. A lookup
  checks the Bloom filter (skip the file if it says "absent"), binary-searches
  the in-memory keys, then seeks to read the value.

- **Bloom filter** (`bloom_filter.h`) — a compact probabilistic set (double
  hashing, ~1% false positives) so a point lookup can skip SSTables that
  definitely do not contain the key, which is what keeps reads cheap as SSTables
  accumulate.

- **LSMStore** (`lsm_store.h`, `lsm_store.cpp`) — ties it together: the write
  path, the newest→oldest read path, MemTable **flush**, **compaction** (merge
  all SSTables, keep the newest value per key, drop tombstones), ordered
  **scan**, and **recovery** (load SSTables + replay the WAL on open).

### Key invariants

- *Newer shadows older.* On a read we stop at the first hit — a newer value, or a
  newer tombstone, hides anything older. This is what makes overwrite and delete
  correct without touching old SSTables.
- *Tombstones are only safe to drop during a full compaction*, when no older
  version of the key can remain.

## Comparison harness

Both engines implement one interface, `KVStore` (`kv_store.h`), so the benchmark
runs the *identical* workload through each:

- **LSMStore** — the new engine.
- **BTreeStore** (`btree_store.h`) — the baseline: a heap file + in-memory B+
  tree, exactly the storage the main engine uses for a table.

Run it with `make lsm-bench`.

## Results

Representative run (`make lsm-bench`, 50,000 keys, 50,000 random updates, 20,000
reads, 100-byte values; absolute numbers vary by machine):

```
== Write throughput ==
  bulk load 50000      LSM=337 ms     B+Tree=60 ms    (B+Tree 5.6x faster)
  50000 updates        LSM=342 ms     B+Tree=352 ms   (LSM 1.03x faster)

== Read latency (20000 point lookups) ==
  hits   (51 SSTables) LSM=104 ms     B+Tree=88 ms    (B+Tree 1.2x faster)
  misses (51 SSTables) LSM=26 ms      B+Tree=3 ms
  LSM after compaction (1 SSTable):  hits=91 ms   misses=0.8 ms

== Space amplification ==
  logical data ~ 5.4 MB (50000 live keys)
  LSM pre-compaction:  11.3 MB, 51 SSTables   (2.09x)
  LSM post-compaction:  5.7 MB,  1 SSTable    (1.06x)
  B+Tree (heap):        5.3 MB                (0.97x)
```

## Analysis

**Write throughput.** Updates are essentially a tie, but the B+ tree baseline
wins clearly on bulk load. This is an honest and instructive result: our baseline
is an unusually *write-friendly* B+ tree — its index lives in memory and its heap
is **append-only**, so it never pays the random in-place page writes that hurt a
classic on-disk B-tree. The LSM, meanwhile, pays a per-operation WAL-serialisation
cost. The textbook "LSM writes much faster" result appears when the baseline's
index is disk-resident and updates are random (B-tree page splits + random I/O);
that penalty simply isn't present in our baseline. We think showing and
explaining this is more valuable than engineering the benchmark to force a win.

**Read latency.** The B+ tree is faster for hits (one in-memory index lookup +
one heap page). The LSM is slower *with many SSTables* because a lookup may probe
several files — classic **read amplification**. The miss numbers make the
mechanisms visible: with 51 SSTables, misses cost 26 ms, but **after compaction
(1 SSTable) they drop to 0.8 ms** — a ~30× improvement — because there is now one
Bloom filter + one index to consult instead of 51.

**Space amplification.** This is where the LSM's behaviour is clearest. After
50,000 updates spread across 51 SSTables, the same logical 5.4 MB occupies
**11.3 MB on disk (2.09×)** because superseded versions and tombstones still sit
in older SSTables. **Compaction reclaims this**, merging everything to a single
5.7 MB SSTable (1.06×). The append-only B+ tree heap stays compact (0.97×) for
this overwrite workload, but it would itself accumulate dead tombstoned slots
under deletes (it has no compaction — noted as future work in the base engine).

## Takeaways

1. LSM converts writes into sequential flushes and defers cleanup to compaction —
   trading **read/space amplification** for write sequentiality.
2. **Compaction is the central knob**: it cuts both read amplification (fewer
   SSTables to probe) and space amplification (drops dead versions). Our numbers
   show both effects directly.
3. **Bloom filters** are what make the multi-SSTable read path tolerable, keeping
   absent-key lookups cheap.
4. Whether LSM beats a B-tree on writes depends entirely on the baseline: against
   an in-memory-indexed append-only heap it does not; against a disk-resident,
   in-place B-tree it would.

## Limitations / future work

- **Single-level compaction.** We merge *all* SSTables into one ("full"
  compaction) rather than the levelled/size-tiered strategies real LSMs use to
  bound write amplification. This is simple to reason about but does more work per
  compaction.
- **No background thread.** Compaction is invoked explicitly (and would run on a
  size trigger in production); here it is a foreground call so its cost is
  observable.
- **Block-level, not byte-level, index.** We keep one index entry per key; a real
  SSTable groups keys into blocks with one index entry per block to shrink the
  in-memory index. The structure is the same, just coarser.
- **Not wired into SQL yet.** The LSM is exercised as a standalone `KVStore` and
  through the benchmark; making it a drop-in table storage for the SQL engine
  (which currently addresses rows by RID) is the natural next step.
