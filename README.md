# MiniDB

A small but complete relational database engine, written from scratch in C++17
for the Advanced Database Management Systems lab project.

MiniDB is not trying to be a feature-rich clone of an existing database. It is a
teaching engine: every required database concept — storage, indexing, query
processing, a cost-based optimizer, transactions and crash recovery — is
implemented in a small amount of readable code that we can actually explain.

```
$ ./minidb mydata
MiniDB shell. Type SQL ending in ';', or .help. .exit to quit.
minidb> CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT);
table 'users' created
minidb> INSERT INTO users VALUES (1,'alice',30),(2,'bob',25),(3,'carol',40);
3 row(s) affected
minidb> SELECT name, age FROM users WHERE age > 26;
+-------+-----+
| name  | age |
+-------+-----+
| alice | 30  |
| carol | 40  |
+-------+-----+
2 row(s)
minidb> EXPLAIN SELECT * FROM users WHERE id = 2;
Query plan:
-> IndexScan(users.users_pk)  point lookup on users.id (unique)  est_rows=1
```

## Features

| Area | What is implemented |
|------|--------------------|
| **Storage engine** | Page-based heap files, a disk (page) manager, and a buffer pool with LRU eviction, pin counts and dirty-page tracking. |
| **Indexing** | A self-balancing **B+ tree** (search / insert / delete with node split, borrow and merge). Automatic primary-key index + optional secondary indexes. |
| **SQL** | `CREATE TABLE`, `CREATE INDEX`, `INSERT`, `SELECT` (with `WHERE` and `JOIN`), `DELETE`, and `BEGIN`/`COMMIT`/`ABORT`. |
| **Query execution** | Volcano (iterator) operators: sequential scan, index scan, filter, projection, nested-loop join and index nested-loop join. |
| **Cost-based optimizer** | Selectivity estimation, table-scan vs index-scan choice, and join-order + join-algorithm selection. `EXPLAIN` shows the chosen plan. |
| **Transactions** | Strict two-phase locking (shared/exclusive row locks) for serializable isolation, with wait-for-graph **deadlock detection**. |
| **Recovery** | Write-ahead logging (WAL) and a simplified ARIES **crash recovery** (analysis → redo → undo) that preserves committed transactions and rolls back the rest. |

## Quick start

```bash
make            # build the `minidb` shell
make test       # build and run the full test suite (59 tests)
make bench      # build and run the benchmark harness
./minidb data   # start an interactive shell using ./data as the storage dir
```

Requirements: a C++17 compiler (clang or g++) and `make`. No external libraries.

See [docs/setup.md](docs/setup.md) for details and a guided demo script.

## Documentation

- [docs/architecture.md](docs/architecture.md) — how the layers fit together, with a diagram and the life of a query.
- [docs/design-decisions.md](docs/design-decisions.md) — the engineering choices we made and the trade-offs behind them.
- [docs/benchmarks.md](docs/benchmarks.md) — performance measurements and analysis.
- [docs/setup.md](docs/setup.md) — build instructions and demonstration walkthroughs (index usage, joins, transactions, deadlock, crash recovery).

## Architecture at a glance

```
            SQL text
               │
        ┌──────▼───────┐
        │   Parser     │  lexer + recursive-descent  →  AST
        └──────┬───────┘
        ┌──────▼───────┐
        │  Optimizer   │  selectivity, scan choice, join order  →  operator tree
        └──────┬───────┘
        ┌──────▼───────┐
        │  Executor    │  Volcano operators (open/next/close)
        └──┬────────┬──┘
           │        │
   ┌───────▼──┐  ┌──▼─────────┐
   │  Indexes │  │  Heap files│        ┌─────────────┐
   │ (B+ tree)│  │            │◄───────│ Transactions│ 2PL + deadlock detection
   └──────────┘  └──┬─────────┘        └─────────────┘
                    │
              ┌─────▼──────┐
              │ Buffer pool│  LRU, pins, dirty bits, write-ahead rule
              └─────┬──────┘
              ┌─────▼──────┐   ┌─────────────┐
              │ Disk mgr   │   │     WAL     │  redo/undo on restart
              └────────────┘   └─────────────┘
```

The source mirrors this structure: `include/minidb/<layer>` for headers and
`src/<layer>` for implementations, with `tests/` holding one test file per
layer.

## Benchmark highlights

Measured on the development machine with `make bench` (20,000 rows):

| Metric | Result |
|--------|--------|
| Insert throughput | ~400k rows/sec (batched in one transaction) |
| Point lookup via PK index | ~0.006 ms/query |
| Point lookup via full scan | ~6.6 ms/query |
| **Index speedup** | **~1000× faster than a full scan** |
| Buffer-pool hit ratio (repeated lookups) | 100% |

Full numbers and discussion are in [docs/benchmarks.md](docs/benchmarks.md).

## Repository layout

```
include/minidb/   public headers, grouped by layer
src/              implementations (+ main.cpp shell, bench_main.cpp)
tests/            unit + integration tests and a tiny test framework
docs/             architecture, design decisions, benchmarks, setup
Makefile          build (uses -MMD header dependency tracking)
```
