// The query executor: a classic Volcano / iterator model.
//
// Every operator implements open() / next() / close(). A parent pulls rows from
// its children one at a time, so a whole query plan streams without
// materialising intermediate results (except where an operator chooses to, e.g.
// the inner side of a nested-loop join).
//
// A row flowing through the tree is an ExecRow: the column values plus, for
// each base relation that contributed, the source (file_id, RID). The sources
// let DELETE find the physical record and let scans take row locks.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "minidb/query/ast.h"
#include "minidb/query/table_handle.h"
#include "minidb/recovery/wal.h"
#include "minidb/rid.h"
#include "minidb/txn/lock_manager.h"
#include "minidb/txn/transaction.h"

namespace minidb {

// Everything an operator needs to touch storage, indexes, locks and the log.
struct ExecContext {
    Transaction* txn = nullptr;   // current transaction (may be null => no locks)
    LockManager* lm = nullptr;
    WAL* wal = nullptr;
    ITableProvider* tables = nullptr;
};

struct RowSource {
    int file_id;
    RID rid;
};

struct ExecRow {
    std::vector<Value> values;
    std::vector<RowSource> sources;  // one per base relation, in schema order
};

// --- predicate evaluation (shared by Filter / joins) ------------------------
// Index of the column in `schema` matching `ref`; throws if absent/ambiguous.
int resolve_in_schema(const std::vector<ColumnRef>& schema, const ColumnRef& ref);
bool compare_values(const Value& a, CompOp op, const Value& b);
bool eval_predicate(const Predicate& p, const ExecRow& row,
                    const std::vector<ColumnRef>& schema);

// --- operator base ----------------------------------------------------------
class Operator {
public:
    virtual ~Operator() = default;
    virtual void open() = 0;
    virtual bool next(ExecRow& out) = 0;
    virtual void close() = 0;

    const std::vector<ColumnRef>& schema() const { return schema_; }

    // One-line description for EXPLAIN, plus child access for the tree walk.
    virtual std::string label() const = 0;
    virtual std::vector<Operator*> children() const { return {}; }

protected:
    std::vector<ColumnRef> schema_;
};

// Pretty-print an operator tree (EXPLAIN).
std::string explain_tree(const Operator* root);

// --- scans ------------------------------------------------------------------
class SeqScan : public Operator {
public:
    SeqScan(ExecContext* ctx, TableHandle* table, const std::string& alias);
    void open() override;
    bool next(ExecRow& out) override;
    void close() override;
    std::string label() const override;

private:
    ExecContext* ctx_;
    TableHandle* table_;
    std::string alias_;
    std::unique_ptr<HeapFile::Iterator> it_;
    std::unique_ptr<HeapFile::Iterator> end_;
};

// Index scan over a key range [lo, hi] (each bound optional / inclusive flag).
class IndexScan : public Operator {
public:
    IndexScan(ExecContext* ctx, TableHandle* table, const std::string& alias,
              const IndexHandle* index, const std::optional<Value>& lo,
              bool lo_inc, const std::optional<Value>& hi, bool hi_inc,
              const std::string& reason);
    void open() override;
    bool next(ExecRow& out) override;
    void close() override;
    std::string label() const override;

private:
    ExecContext* ctx_;
    TableHandle* table_;
    std::string alias_;
    const IndexHandle* index_;
    std::optional<Value> lo_, hi_;
    bool lo_inc_, hi_inc_;
    std::string reason_;
    std::vector<std::pair<Value, RID>> matches_;
    std::size_t pos_ = 0;
};

// --- filter -----------------------------------------------------------------
class Filter : public Operator {
public:
    Filter(std::unique_ptr<Operator> child, std::vector<Predicate> preds);
    void open() override;
    bool next(ExecRow& out) override;
    void close() override;
    std::string label() const override;
    std::vector<Operator*> children() const override { return {child_.get()}; }

private:
    std::unique_ptr<Operator> child_;
    std::vector<Predicate> preds_;
};

// --- projection -------------------------------------------------------------
class Project : public Operator {
public:
    Project(std::unique_ptr<Operator> child, std::vector<ColumnRef> cols);
    void open() override;
    bool next(ExecRow& out) override;
    void close() override;
    std::string label() const override;
    std::vector<Operator*> children() const override { return {child_.get()}; }

private:
    std::unique_ptr<Operator> child_;
    std::vector<int> indices_;  // child column index for each output column
};

// --- nested-loop join (inner side materialised once) ------------------------
class NestedLoopJoin : public Operator {
public:
    NestedLoopJoin(std::unique_ptr<Operator> outer,
                   std::unique_ptr<Operator> inner, Predicate on);
    void open() override;
    bool next(ExecRow& out) override;
    void close() override;
    std::string label() const override;
    std::vector<Operator*> children() const override {
        return {outer_.get(), inner_.get()};
    }

private:
    std::unique_ptr<Operator> outer_;
    std::unique_ptr<Operator> inner_;
    Predicate on_;
    std::vector<ExecRow> inner_rows_;  // materialised inner side
    ExecRow cur_outer_;
    bool have_outer_ = false;
    std::size_t inner_pos_ = 0;
};

// --- index nested-loop join (probe inner's index per outer row) -------------
class IndexNestedLoopJoin : public Operator {
public:
    IndexNestedLoopJoin(ExecContext* ctx, std::unique_ptr<Operator> outer,
                        TableHandle* inner_table, const std::string& inner_alias,
                        const IndexHandle* inner_index, Predicate on);
    void open() override;
    bool next(ExecRow& out) override;
    void close() override;
    std::string label() const override;
    std::vector<Operator*> children() const override { return {outer_.get()}; }

private:
    bool advance_outer();

    ExecContext* ctx_;
    std::unique_ptr<Operator> outer_;
    TableHandle* inner_table_;
    std::string inner_alias_;
    const IndexHandle* inner_index_;
    Predicate on_;
    int outer_key_idx_ = -1;          // outer column index for the join key
    std::vector<ColumnRef> inner_schema_;
    ExecRow cur_outer_;
    std::vector<RID> inner_matches_;
    std::size_t inner_pos_ = 0;
};

// --- statement-level executors (return counts / results) --------------------
struct SelectResult {
    std::vector<std::string> columns;        // header
    std::vector<std::vector<Value>> rows;
};

// Drains an operator tree into a SelectResult.
SelectResult run_select(Operator* root);

// INSERT: validates rows, inserts into the heap (logged), updates indexes,
// takes write locks and records undo. Returns the number of rows inserted.
int run_insert(ExecContext* ctx, TableHandle* table, const InsertStmt& stmt);

// DELETE: scans (optionally filtered), takes write locks, deletes (logged),
// updates indexes and records undo. Returns the number of rows deleted.
int run_delete(ExecContext* ctx, TableHandle* table,
               const std::vector<Predicate>& where);

}  // namespace minidb
