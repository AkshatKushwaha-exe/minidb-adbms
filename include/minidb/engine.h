// The database engine: the public entry point that wires every layer together
// (catalog, storage, buffer pool, indexes, WAL/recovery, locking, query
// processing) and runs SQL statements.
//
// Lifecycle:
//   * On construction it loads the catalog, opens each table's heap file,
//     runs crash recovery against the WAL, and (re)builds the in-memory B+ tree
//     indexes by scanning the recovered heaps.
//   * execute() parses one SQL statement and runs it, wrapping DML in a
//     transaction (auto-commit, unless a BEGIN..COMMIT block is open).
//   * On destruction it flushes the buffer pool and saves the catalog.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "minidb/catalog/catalog.h"
#include "minidb/query/executor.h"
#include "minidb/query/table_handle.h"
#include "minidb/recovery/wal.h"
#include "minidb/storage/buffer_pool.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/txn/lock_manager.h"
#include "minidb/txn/transaction_manager.h"

namespace minidb {

struct QueryResult {
    enum class Kind { SELECT, MODIFY, MESSAGE, EXPLAIN };
    Kind kind = Kind::MESSAGE;
    SelectResult select;     // valid for SELECT
    int affected = 0;        // valid for MODIFY
    std::string message;     // valid for MESSAGE / EXPLAIN
};

class Engine : public ITableProvider {
public:
    explicit Engine(const std::string& data_dir,
                    std::size_t buffer_pool_size = DEFAULT_BUFFER_POOL_SIZE);
    ~Engine();

    // Parse and run one SQL statement. Throws DBException subclasses on error.
    QueryResult execute(const std::string& sql);

    // ITableProvider
    TableHandle* get_table(const std::string& name) override;

    // Stats accessors (used by benchmarks / the CLI).
    BufferPool& buffer_pool() { return *bpool_; }
    bool in_transaction() const { return current_txn_ != nullptr; }
    std::vector<std::string> table_names() const { return catalog_.table_names(); }

    // FOR TESTS / DEMOS ONLY: simulate a crash by discarding all in-memory
    // (dirty) pages without flushing them. After calling this, destroy the
    // Engine and construct a new one on the same directory to see recovery
    // rebuild the committed state from the WAL.
    void simulate_crash() { crashed_ = true; }

private:
    // One table's runtime state: storage + indexes + the handle we hand out.
    struct TableRuntime {
        TableInfo info;
        std::unique_ptr<DiskManager> disk;
        std::unique_ptr<HeapFile> heap;
        std::vector<std::unique_ptr<BTree>> trees;  // parallel to info.indexes
        TableHandle handle;
    };

    void open();
    void open_table_storage(const TableInfo& info);  // disk + heap, no indexes
    void build_indexes(TableRuntime* rt);            // scan heap -> B+ trees
    void rebuild_handle(TableRuntime* rt);

    void create_table(const CreateTableStmt& s, QueryResult& out);
    void create_index(const CreateIndexStmt& s, QueryResult& out);

    // Runs `fn` inside a transaction (the open session txn, or a fresh
    // auto-commit one). Handles commit/abort and deadlock cleanup.
    template <typename Fn>
    void with_txn(Fn&& fn);

    void apply_undo(const UndoAction& a);

    std::string data_dir_;
    Catalog catalog_;
    std::unique_ptr<WAL> wal_;
    std::unique_ptr<BufferPool> bpool_;
    std::unique_ptr<LockManager> lock_mgr_;
    std::unique_ptr<TransactionManager> txn_mgr_;

    std::map<std::string, std::unique_ptr<TableRuntime>> tables_;
    std::map<int, TableRuntime*> by_file_id_;

    Transaction* current_txn_ = nullptr;  // open BEGIN..COMMIT session, or null
    bool crashed_ = false;                 // if set, destructor skips flushing
};

}  // namespace minidb
