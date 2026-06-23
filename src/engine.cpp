#include "minidb/engine.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "minidb/exceptions.h"
#include "minidb/query/optimizer.h"
#include "minidb/query/parser.h"
#include "minidb/recovery/recovery_manager.h"

namespace minidb {

static std::string catalog_path(const std::string& dir) {
    return dir + "/catalog.meta";
}
static std::string wal_path(const std::string& dir) { return dir + "/wal.log"; }
static std::string table_path(const std::string& dir, int file_id) {
    return dir + "/table_" + std::to_string(file_id) + ".db";
}

Engine::Engine(const std::string& data_dir, std::size_t buffer_pool_size)
    : data_dir_(data_dir) {
    std::filesystem::create_directories(data_dir_);  // ensure the dir exists
    bpool_ = std::make_unique<BufferPool>(buffer_pool_size);
    wal_ = std::make_unique<WAL>(wal_path(data_dir_));
    lock_mgr_ = std::make_unique<LockManager>();
    txn_mgr_ = std::make_unique<TransactionManager>(
        wal_.get(), lock_mgr_.get(),
        [this](const UndoAction& a) { apply_undo(a); });
    open();
}

Engine::~Engine() {
    if (crashed_) {
        // Simulated crash: drop dirty pages on the floor (do NOT flush). The
        // WAL on disk (committed records were already flushed) is the only
        // durable state, so recovery must rebuild from it next time.
        return;
    }
    // Clean shutdown: flush everything and persist the catalog.
    if (bpool_) bpool_->flush_all();
    if (wal_) wal_->flush();
    catalog_.save(catalog_path(data_dir_));
}

void Engine::open() {
    catalog_.load(catalog_path(data_dir_));

    // 1. Open storage for every known table (no indexes yet).
    for (const auto& name : catalog_.table_names()) {
        open_table_storage(catalog_.get_table(name));
    }

    // 2. Write-ahead rule: the buffer pool must flush the log before a page.
    bpool_->set_log_flush_callback(
        [this](lsn_t target) { wal_->flush_to_lsn(target); });

    // 3. Crash recovery against the WAL (operates on the heap pages).
    RecoveryManager rec(bpool_.get(), wal_path(data_dir_));
    rec.recover();

    // 4. Rebuild indexes from the recovered heaps (indexes are derived data).
    for (auto& kv : tables_) build_indexes(kv.second.get());

    // 5. Seed the transaction id counter so ids never collide across restarts.
    txn_id_t max_txn = 0;
    for (const auto& r : WAL::read_all(wal_path(data_dir_)))
        max_txn = std::max(max_txn, r.txn);
    txn_mgr_->set_next_txn_id(max_txn + 1);
}

void Engine::open_table_storage(const TableInfo& info) {
    auto rt = std::make_unique<TableRuntime>();
    rt->info = info;
    rt->disk = std::make_unique<DiskManager>(table_path(data_dir_, info.id));
    bpool_->register_file_with_id(info.id, rt->disk.get());
    rt->heap = std::make_unique<HeapFile>(bpool_.get(), info.id, wal_.get());
    tables_[info.name] = std::move(rt);
    by_file_id_[info.id] = tables_[info.name].get();
}

void Engine::build_indexes(TableRuntime* rt) {
    const Schema& schema = rt->info.schema;
    rt->trees.clear();
    for (std::size_t i = 0; i < rt->info.indexes.size(); ++i) {
        rt->trees.push_back(std::make_unique<BTree>());
    }
    // One scan of the heap populates every index.
    for (auto it = rt->heap->begin(); it != rt->heap->end(); ++it) {
        auto pr = *it;
        Tuple tuple = schema.deserialize(pr.second);
        for (std::size_t i = 0; i < rt->info.indexes.size(); ++i) {
            int col = rt->info.indexes[i].column;
            rt->trees[i]->insert(tuple[col], pr.first);
        }
    }
    rebuild_handle(rt);
}

void Engine::rebuild_handle(TableRuntime* rt) {
    TableHandle& h = rt->handle;
    h.name = rt->info.name;
    h.file_id = rt->info.id;
    h.schema = &rt->info.schema;
    h.heap = rt->heap.get();
    h.indexes.clear();
    for (std::size_t i = 0; i < rt->info.indexes.size(); ++i) {
        const IndexInfo& ii = rt->info.indexes[i];
        h.indexes.push_back(
            {ii.name, ii.column, ii.unique, ii.primary, rt->trees[i].get()});
    }
}

TableHandle* Engine::get_table(const std::string& name) {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : &it->second->handle;
}

// --- DDL --------------------------------------------------------------------
void Engine::create_table(const CreateTableStmt& s, QueryResult& out) {
    // Build the schema; find the primary-key column (default: first column).
    std::vector<Column> cols;
    int pk = 0;
    for (std::size_t i = 0; i < s.columns.size(); ++i) {
        cols.push_back({s.columns[i].name, s.columns[i].type});
        if (s.columns[i].primary_key) pk = static_cast<int>(i);
    }
    Schema schema(cols, pk);
    catalog_.create_table(s.table, schema);  // also creates the pk index entry

    open_table_storage(catalog_.get_table(s.table));
    build_indexes(by_file_id_[catalog_.get_table(s.table).id]);
    catalog_.save(catalog_path(data_dir_));

    out.kind = QueryResult::Kind::MESSAGE;
    out.message = "table '" + s.table + "' created";
}

void Engine::create_index(const CreateIndexStmt& s, QueryResult& out) {
    if (!catalog_.has_table(s.table))
        throw CatalogException("no such table '" + s.table + "'");
    const TableInfo& info = catalog_.get_table(s.table);
    int col = info.schema.column_index(s.column);
    if (col < 0)
        throw CatalogException("no such column '" + s.column + "'");

    catalog_.add_index(s.table, {s.index_name, col, /*unique=*/false,
                                 /*primary=*/false});

    // Update the runtime: copy the new catalog info and rebuild indexes.
    TableRuntime* rt = tables_[s.table].get();
    rt->info = catalog_.get_table(s.table);
    build_indexes(rt);
    catalog_.save(catalog_path(data_dir_));

    out.kind = QueryResult::Kind::MESSAGE;
    out.message = "index '" + s.index_name + "' created on " + s.table + "(" +
                  s.column + ")";
}

// --- transaction helper -----------------------------------------------------
template <typename Fn>
void Engine::with_txn(Fn&& fn) {
    bool auto_commit = (current_txn_ == nullptr);
    Transaction* txn = auto_commit ? txn_mgr_->begin() : current_txn_;
    try {
        fn(txn);
    } catch (...) {
        // Roll back: an auto-commit txn always; a session txn is also aborted
        // (e.g. on deadlock) and the session is closed.
        txn_mgr_->abort(txn);
        if (!auto_commit) current_txn_ = nullptr;
        throw;
    }
    if (auto_commit) txn_mgr_->commit(txn);
}

void Engine::apply_undo(const UndoAction& a) {
    TableRuntime* rt = by_file_id_[a.file_id];
    if (!rt) return;
    const Schema& schema = rt->info.schema;
    lsn_t lsn = wal_->last_lsn();
    if (a.was_insert) {
        // Undo an insert: remove its index entries, then tombstone the row.
        std::vector<uint8_t> bytes;
        if (rt->heap->get(a.rid, bytes)) {
            Tuple t = schema.deserialize(bytes);
            for (std::size_t i = 0; i < rt->info.indexes.size(); ++i)
                rt->trees[i]->erase(t[rt->info.indexes[i].column], a.rid);
        }
        rt->heap->remove_at(a.rid, lsn);
    } else {
        // Undo a delete: put the row back and re-add its index entries.
        rt->heap->insert_at(a.rid, a.image, lsn);
        Tuple t = schema.deserialize(a.image);
        for (std::size_t i = 0; i < rt->info.indexes.size(); ++i)
            rt->trees[i]->insert(t[rt->info.indexes[i].column], a.rid);
    }
}

// --- statement dispatch -----------------------------------------------------
QueryResult Engine::execute(const std::string& sql) {
    // EXPLAIN <select> : show the plan without running it.
    std::string trimmed = sql;
    std::size_t start = trimmed.find_first_not_of(" \t\n\r");
    bool explain_only = false;
    if (start != std::string::npos) {
        std::string head = trimmed.substr(start, 7);
        std::string up = head;
        for (char& c : up) c = static_cast<char>(std::toupper((unsigned char)c));
        if (up == "EXPLAIN") {
            explain_only = true;
            trimmed = trimmed.substr(start + 7);
        }
    }

    Statement st = Parser::parse(trimmed);
    QueryResult out;

    switch (st.type) {
        case StmtType::CREATE_TABLE:
            create_table(st.create_table, out);
            return out;
        case StmtType::CREATE_INDEX:
            create_index(st.create_index, out);
            return out;
        case StmtType::BEGIN:
            if (current_txn_) throw DBException("a transaction is already open");
            current_txn_ = txn_mgr_->begin();
            out.message = "transaction started";
            return out;
        case StmtType::COMMIT:
            if (!current_txn_) throw DBException("no transaction to commit");
            txn_mgr_->commit(current_txn_);
            current_txn_ = nullptr;
            out.message = "committed";
            return out;
        case StmtType::ABORT:
            if (!current_txn_) throw DBException("no transaction to abort");
            txn_mgr_->abort(current_txn_);
            current_txn_ = nullptr;
            out.message = "aborted";
            return out;
        case StmtType::INSERT: {
            TableHandle* h = get_table(st.insert.table);
            if (!h) throw CatalogException("no such table '" + st.insert.table + "'");
            with_txn([&](Transaction* txn) {
                ExecContext ctx{txn, lock_mgr_.get(), wal_.get(), this};
                out.affected = run_insert(&ctx, h, st.insert);
            });
            out.kind = QueryResult::Kind::MODIFY;
            return out;
        }
        case StmtType::DELETE: {
            TableHandle* h = get_table(st.del.table);
            if (!h) throw CatalogException("no such table '" + st.del.table + "'");
            with_txn([&](Transaction* txn) {
                ExecContext ctx{txn, lock_mgr_.get(), wal_.get(), this};
                out.affected = run_delete(&ctx, h, st.del.where);
            });
            out.kind = QueryResult::Kind::MODIFY;
            return out;
        }
        case StmtType::SELECT: {
            with_txn([&](Transaction* txn) {
                ExecContext ctx{txn, lock_mgr_.get(), wal_.get(), this};
                auto plan = Optimizer::build_select(&ctx, st.select);
                if (explain_only) {
                    out.kind = QueryResult::Kind::EXPLAIN;
                    out.message = explain_tree(plan.get());
                } else {
                    out.kind = QueryResult::Kind::SELECT;
                    out.select = run_select(plan.get());
                }
            });
            return out;
        }
    }
    throw DBException("unhandled statement type");
}

}  // namespace minidb
