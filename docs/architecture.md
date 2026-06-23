# Architecture

This document explains how MiniDB is put together: the layers, how they depend
on each other, and what happens end-to-end when a query runs.

## Layering

MiniDB is organised as a stack of layers, each depending only on the ones below
it. This keeps responsibilities clear and made the system testable one layer at
a time.

```
   Query layer      parser → optimizer → executor
   ────────────────────────────────────────────────
   Transactions     transaction manager, lock manager (2PL)
   Recovery         WAL, recovery manager (ARIES-lite)
   ────────────────────────────────────────────────
   Access methods   B+ tree indexes
   Records          schema, tuple (de)serialisation, catalog
   ────────────────────────────────────────────────
   Storage          heap files → buffer pool → disk manager → page
```

Header files live under `include/minidb/<layer>/` and implementations under
`src/<layer>/`. The `Engine` class (`src/engine.cpp`) is the conductor that owns
one instance of each component and wires them together.

## The storage layer

**Page** (`storage/page.h`). The fixed-size (4 KiB) unit of storage. We use a
*slotted page*: a header, then a slot directory growing from the front, then
record bytes growing from the back. A slot stores `(offset, length)`; a length
of 0 marks a deleted record (tombstone). This is exactly how PostgreSQL and
SQLite store variable-length tuples, and it lets every record be addressed by a
small slot number (`RID = (page_id, slot)`), which is what the B+ tree stores.

**Disk manager** (`storage/disk_manager.h`). Owns one file and does all I/O in
whole pages: `allocate_page`, `read_page`, `write_page`. Page id = byte offset /
page size.

**Buffer pool** (`storage/buffer_pool.h`). The in-memory page cache. It has a
fixed number of frames and evicts the least-recently-used **unpinned** page when
full. Each frame tracks a pin count (a pinned page is in use and cannot be
evicted) and a dirty bit (only modified pages are written back). Before any
dirty page is written to disk, the buffer pool calls back into the WAL to flush
the log up to that page's LSN — this enforces the write-ahead rule.

**Heap file** (`storage/heap_file.h`). A table's records spread across pages.
`insert` appends to a page with free space (allocating a new page when needed),
`remove` tombstones a slot, and an iterator walks every live record (used by
sequential scans). If a logger is attached, inserts/deletes write a WAL record
and stamp the page LSN first.

## Records, schema and catalog

A **Schema** (`record/schema.h`) is the list of columns (each `INT` or `TEXT`)
plus which column is the primary key. It serialises a tuple to the bytes stored
in a slot and back (INT → 8 bytes; TEXT → 4-byte length + bytes).

The **Catalog** (`catalog/catalog.h`) is the data dictionary: it remembers every
table's schema and indexes, and persists them to a small text file
(`catalog.meta`). Each table gets a stable integer id at creation; that id is
what the WAL records, so recovery always maps a log record back to the right
table no matter how many tables exist later.

## Indexing

The **B+ tree** (`index/btree.h`) maps a key (a `Value`) to one or more RIDs.
Inserts split full nodes; deletes rebalance underfull nodes by borrowing from a
sibling or merging. Leaves are linked so range scans are a single walk along the
leaf chain.

Indexes are kept **in memory** and treated as *derived* data: the engine rebuilds
every B+ tree by scanning the (already recovered) heap at startup. This means we
never have to log or recover indexes separately — see
[design-decisions.md](design-decisions.md).

## Recovery

The **WAL** (`recovery/wal.h`) appends a record for every change
(`BEGIN/INSERT/DELETE/COMMIT/ABORT`). A commit is not acknowledged until its log
record is on disk. The **recovery manager** (`recovery/recovery_manager.h`) runs
a simplified ARIES on restart:

1. **Analysis** — scan the log to find which transactions committed.
2. **Redo** — replay every logged change (a page-LSN guard skips changes the
   page already reflects, making redo idempotent).
3. **Undo** — for every transaction that did not commit, apply the inverse of
   its changes in reverse order.

The result: committed transactions are preserved and uncommitted ones leave no
trace.

## Transactions

The **lock manager** (`txn/lock_manager.h`) implements two-phase locking with
shared/exclusive locks at row granularity. When a transaction would have to wait,
it adds edges to a wait-for graph and looks for a cycle; if one exists the waiter
is chosen as the victim and aborts (`DeadlockException`). The **transaction
manager** (`txn/transaction_manager.h`) hands out transactions, drives
commit/abort, and on abort replays the transaction's in-memory undo list to roll
back its own changes. Locks are released only at commit/abort (strict 2PL).

## Query processing

**Parser** (`query/parser.h`). A hand-written lexer + recursive-descent parser
turns SQL text into an AST (`query/ast.h`).

**Optimizer** (`query/optimizer.h`). Builds a physical operator tree from the
AST. It estimates selectivity, chooses an index scan vs a full table scan per
table, and orders joins (smallest relation first) choosing an index nested-loop
join when the inner relation's join column is indexed.

**Executor** (`query/executor.h`). Classic Volcano model: every operator
implements `open` / `next` / `close`, and a parent pulls rows from its children
one at a time. Operators: `SeqScan`, `IndexScan`, `Filter`, `Project`,
`NestedLoopJoin`, `IndexNestedLoopJoin`, plus statement runners for INSERT and
DELETE.

## The life of a query

`SELECT u.name, o.item FROM users u JOIN orders o ON u.id = o.uid WHERE u.name = 'alice';`

1. The **engine** wraps the statement in a transaction (auto-commit, unless a
   `BEGIN` block is open).
2. The **parser** produces a `SelectStmt` AST.
3. The **optimizer** resolves the two relations, pushes `u.name = 'alice'` down
   to the `users` scan, and — because `users` is small and `orders` has a usable
   key — builds, say, an index nested-loop join. `EXPLAIN` prints this tree.
4. The **executor** opens the tree. Each base scan reads pages through the
   **buffer pool** (taking shared **locks** on the rows it returns) and
   deserialises tuples with the **schema**. The join probes the index / inner
   rows; the filter drops non-matching rows; the projection keeps the requested
   columns.
5. Rows stream up to the engine, which returns them. On success the transaction
   commits (flushing its WAL records); on error it aborts and rolls back.

## Why this shape

Every arrow in the layer diagram points downward, so each layer can be
understood and tested in isolation (`tests/test_storage.cpp`,
`tests/test_btree.cpp`, … up to `tests/test_engine.cpp` which exercises the whole
stack through SQL). That separation is what let us build and verify the system
incrementally rather than all at once.
