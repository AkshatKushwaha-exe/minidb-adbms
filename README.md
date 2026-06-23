# MiniDB

A small but complete relational database engine, written from scratch in C++17
for the Advanced Database Management Systems lab project — including an
**LSM-tree storage extension (Track C)**.

```
minidb> CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT);
minidb> INSERT INTO users VALUES (1,'alice',30),(2,'bob',25),(3,'carol',40);
minidb> SELECT name, age FROM users WHERE age > 26;
+-------+-----+
| name  | age |
+-------+-----+
| alice | 30  |
| carol | 40  |
+-------+-----+
minidb> EXPLAIN SELECT * FROM users WHERE id = 2;
-> IndexScan(users.users_pk)  point lookup on users.id (unique)  est_rows=1
```

---

## 1. Project Overview

**Problem.** Build a working database engine from foundational components —
storage, indexing, query processing, a cost-based optimizer, transactions and
crash recovery — and demonstrate understanding of database internals rather than
feature count.

**Goals.** Correctness, completeness, clean system design, and code we can fully
explain. Every component is small and readable on purpose.

**Chosen extension track.** **Track C — Modern Storage (LSM-tree).** We implement
an LSM-tree storage engine (MemTable → SSTables → compaction, with a Bloom
filter) and benchmark it against the base engine's B+ tree/heap storage. See
section 9 and [docs/extension-lsm.md](docs/extension-lsm.md).

## 2. System Architecture

```
            SQL text
               │
        ┌──────▼───────┐
        │   Parser     │  lexer + recursive descent  →  AST
        ├──────────────┤
        │  Optimizer   │  selectivity, scan choice, join order  →  operator tree
        ├──────────────┤
        │  Executor    │  Volcano operators (open / next / close)
        └──┬────────┬──┘
           │        │
   ┌───────▼──┐  ┌──▼─────────┐     ┌─────────────┐
   │  Indexes │  │  Heap files│ ◄───│ Transactions│  2PL + deadlock detection
   │ (B+ tree)│  │            │     └─────────────┘
   └──────────┘  └──┬─────────┘
                    │
              ┌─────▼──────┐
              │ Buffer pool│  LRU, pins, dirty bits, write-ahead rule
              └─────┬──────┘
       ┌────────────▼─────┐   ┌─────────────┐
       │   Disk manager   │   │     WAL     │  redo / undo on restart
       └──────────────────┘   └─────────────┘

   Extension:  LSMStore  =  MemTable + WAL  ──flush──►  SSTables ──compaction──►
```

**Major modules** (headers in `include/minidb/<layer>`, code in `src/<layer>`):
`storage/` (page, disk manager, buffer pool, heap file), `record/` + `catalog/`
(schema, tuples, data dictionary), `index/` (B+ tree), `recovery/` (WAL +
recovery), `txn/` (lock manager, transaction manager), `query/` (parser,
optimizer, executor), `engine.cpp` (the conductor), and `lsm/` (the extension).

**Data flow.** SQL → parser → AST → optimizer → operator tree → executor, which
reads/writes pages through the buffer pool (taking locks, logging to the WAL) and
consults the B+ tree indexes. Full walkthrough in
[docs/architecture.md](docs/architecture.md).

## 3. Storage Layer

- **Page format** (`storage/page.h`) — fixed 4 KiB **slotted page**: an 8-byte
  page-LSN + slot count + free pointer, a slot directory growing from the front,
  and record bytes growing from the back. A zero-length slot is a tombstone.
  Records are addressed by `RID = (page_id, slot)`.
- **Heap files** (`storage/heap_file.h`) — a table's records across pages; insert
  appends to a page with room (or a new page), delete tombstones a slot, and an
  iterator walks all live records.
- **Buffer pool** (`storage/buffer_pool.h`) — fixed-frame page cache with **LRU**
  eviction, **pin counts** (a pinned page can't be evicted) and a **dirty bit**
  (only modified pages are written back). It enforces the **write-ahead rule**:
  the WAL is flushed up to a page's LSN before that page hits disk.

## 4. Indexing

- **B+ tree design** (`index/btree.h`) — an in-memory tree mapping a key
  (`Value`) → one or more `RID`s; the primary-key index is unique, secondary
  indexes allow duplicates. Indexes are *derived* data, rebuilt by scanning the
  heap at startup (so recovery only deals with the heap).
- **Node structure** — internal nodes hold separator keys + child pointers; leaf
  nodes hold keys + RID lists and are linked for range scans. Inserts split full
  nodes; deletes rebalance underfull nodes by **borrowing or merging**.
- **Search path** — descend from the root following separators to a leaf, then
  binary-search the leaf; range scans walk the leaf chain. Validated against a
  `std::map` oracle over 4,000 randomised operations.

## 5. Query Execution

- **Parser** (`query/parser.h`) — a hand-written lexer + recursive-descent parser
  for `CREATE TABLE/INDEX`, `INSERT`, `SELECT` (WHERE/JOIN), `DELETE`, and
  `BEGIN/COMMIT/ABORT`, producing an AST (`query/ast.h`).
- **Query plan generation** — the optimizer turns the AST into a physical
  operator tree (section 6); `EXPLAIN <select>` prints it.
- **Operator execution** (`query/executor.h`) — the **Volcano / iterator** model:
  `SeqScan`, `IndexScan`, `Filter`, `Project`, `NestedLoopJoin`,
  `IndexNestedLoopJoin`, plus statement runners for INSERT/DELETE. Each operator
  implements `open`/`next`/`close` and a parent pulls rows one at a time.

## 6. Optimizer

- **Cost estimation** (`query/optimizer.h`) — exact row counts come from the
  primary index size; access paths are costed as full-scan (≈ N rows) vs
  index-scan (≈ selectivity × N).
- **Selectivity estimation** — equality on a unique/primary column ≈ 1 row,
  equality on a non-unique column ≈ 10%, a range ≈ 33%.
- **Join ordering** — relations are ordered smallest-first (greedy) and each join
  is attached via a connecting predicate, choosing an **index nested-loop join**
  when the inner relation's join column is indexed, else a nested-loop join.

## 7. Transactions & Concurrency

- **Locking strategy** (`txn/lock_manager.h`) — **strict two-phase locking** with
  shared/exclusive **row-level** locks; all locks are released only at
  commit/abort.
- **Isolation guarantees** — strict 2PL provides **serializable** isolation (and
  recoverability — no cascading aborts).
- **Deadlock handling** — a **wait-for graph** is checked for a cycle whenever a
  transaction would block; the transaction that closes the cycle is chosen as the
  victim and aborted (`DeadlockException`), rolling back via its undo list.

## 8. Recovery

- **WAL design** (`recovery/wal.h`) — an append-only log; a `COMMIT` is not
  acknowledged until its record is durable (write-ahead logging).
- **Log records** (`recovery/log_record.h`) — `BEGIN`, `INSERT` (after-image),
  `DELETE` (before-image), `COMMIT`, `ABORT`, `CHECKPOINT`, each with an LSN.
- **Crash recovery procedure** (`recovery/recovery_manager.h`) — a simplified
  **ARIES**: *analysis* (find committed txns) → *redo* (replay all changes; a
  page-LSN guard makes redo idempotent) → *undo* (roll back uncommitted txns).
  Committed work survives; uncommitted work leaves no trace.

## 9. Extension Track — LSM-tree storage (Track C)

- **Motivation.** The base engine updates data in place (read-optimised). An
  LSM-tree never updates in place: writes are absorbed in memory and flushed as
  immutable sorted files, turning writes into sequential I/O (the RocksDB /
  LevelDB approach).
- **Design** (`lsm/`). **MemTable** (sorted in-memory map + tombstones) backed by
  a **WAL**; flushed to immutable **SSTables** (sorted on disk, in-memory
  key→offset index + **Bloom filter**); **compaction** merges SSTables, keeping
  the newest value per key and dropping tombstones. Reads check MemTable then
  SSTables newest→oldest.
- **Results** (`make lsm-bench`, vs the B+ tree/heap baseline): compaction cuts
  **space amplification from 2.09× to 1.06×** (51 SSTables → 1) and **miss-lookup
  latency from ~26 ms to ~0.8 ms**; updates are on par with the baseline. Full
  analysis (including why bulk-load favours our append-only baseline) is in
  [docs/extension-lsm.md](docs/extension-lsm.md).

## 10. Benchmarks

- **Experimental setup.** Two harnesses: `make bench` (the row-store engine:
  insert throughput, index vs full scan, buffer-pool hit ratio, join) and
  `make lsm-bench` (LSM vs B+ tree/heap on an identical KV workload: write
  throughput, read latency, space amplification).
- **Results (row store).** ~400k inserts/sec; PK index lookup **~1000× faster**
  than a full scan; 100% buffer-pool hit ratio on a hot row.
- **Results (extension).** See section 9.
- **Analysis.** [docs/benchmarks.md](docs/benchmarks.md) and
  [docs/extension-lsm.md](docs/extension-lsm.md).

## 11. Limitations

- **Missing features.** No `UPDATE` statement (model as DELETE+INSERT), no
  aggregation/`GROUP BY`/`ORDER BY`, no `NULL`s, two column types (INT, TEXT).
- **Scalability limits.** Indexes are in-memory (rebuilt at startup); the lock
  table uses a single global latch; heap inserts append (deleted space in earlier
  pages isn't reclaimed — no heap compaction); recovery replays the whole log
  (no checkpoint-based truncation).
- **Future improvements.** Persisted/disk-resident B+ tree, an `UPDATE` path and
  aggregation, MVCC for higher read concurrency, levelled LSM compaction with a
  background thread, and wiring the LSM in as a drop-in table storage.

## 12. How to Run

**Dependencies.** A C++17 compiler (clang or g++) and GNU `make`. No external
libraries.

**Build & test.**
```bash
make            # build the ./minidb interactive shell
make test       # build + run all 70 tests
make bench      # row-store benchmark
make lsm-bench  # extension benchmark (LSM vs B+ tree)
make clean
```

**Example session.**
```bash
./minidb mydata        # data dir (created if absent)
```
```sql
CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT);
INSERT INTO users VALUES (1,'alice',30),(2,'bob',25);
SELECT * FROM users WHERE age > 26;
CREATE INDEX idx_age ON users (age);
EXPLAIN SELECT * FROM users WHERE age = 25;
BEGIN; DELETE FROM users WHERE id = 2; COMMIT;
.tables   .stats   .exit
```

Guided demos for index usage, joins, transactions, deadlock and crash recovery
are in [docs/setup.md](docs/setup.md). Design rationale and trade-offs are in
[docs/design-decisions.md](docs/design-decisions.md).

## Repository layout

```
include/minidb/   headers by layer (storage, index, query, txn, recovery, lsm, …)
src/              implementations (+ main.cpp shell, bench_main.cpp)
benchmarks/       lsm_benchmark.cpp (extension comparison)
tests/            unit + integration tests and a tiny test framework
docs/             architecture, design decisions, benchmarks, setup, extension-lsm
Makefile          build (header-dependency tracking via -MMD)
```
