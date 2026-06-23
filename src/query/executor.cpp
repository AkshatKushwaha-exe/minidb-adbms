#include "minidb/query/executor.h"

#include <sstream>

#include "minidb/exceptions.h"

namespace minidb {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------
int resolve_in_schema(const std::vector<ColumnRef>& schema,
                      const ColumnRef& ref) {
    int found = -1;
    for (std::size_t i = 0; i < schema.size(); ++i) {
        if (schema[i].col != ref.col) continue;
        if (!ref.table.empty() && schema[i].table != ref.table) continue;
        if (found != -1)
            throw SQLException("ambiguous column reference '" + ref.col + "'");
        found = static_cast<int>(i);
    }
    if (found == -1)
        throw SQLException("unknown column '" + ref.col + "'");
    return found;
}

bool compare_values(const Value& a, CompOp op, const Value& b) {
    switch (op) {
        case CompOp::EQ: return a == b;
        case CompOp::NE: return a != b;
        case CompOp::LT: return a < b;
        case CompOp::LE: return a <= b;
        case CompOp::GT: return a > b;
        case CompOp::GE: return a >= b;
    }
    return false;
}

bool eval_predicate(const Predicate& p, const ExecRow& row,
                    const std::vector<ColumnRef>& schema) {
    const Value& left = row.values[resolve_in_schema(schema, p.left)];
    if (p.right_is_column) {
        const Value& right = row.values[resolve_in_schema(schema, p.right_col)];
        return compare_values(left, p.op, right);
    }
    return compare_values(left, p.op, p.right_value);
}

namespace {
// Take a shared lock on a row if we are running inside a transaction.
void lock_shared(ExecContext* ctx, int file_id, const RID& rid) {
    if (ctx && ctx->txn && ctx->lm) {
        ctx->lm->lock_shared(ctx->txn, Resource{file_id, rid});
    }
}
void lock_exclusive(ExecContext* ctx, int file_id, const RID& rid) {
    if (ctx && ctx->txn && ctx->lm) {
        ctx->lm->lock_exclusive(ctx->txn, Resource{file_id, rid});
    }
}

std::vector<ColumnRef> schema_for(const TableHandle* table,
                                  const std::string& alias) {
    std::string a = alias.empty() ? table->name : alias;
    std::vector<ColumnRef> cols;
    for (const auto& c : table->schema->columns()) cols.push_back({a, c.name});
    return cols;
}
}  // namespace

// ---------------------------------------------------------------------------
// EXPLAIN
// ---------------------------------------------------------------------------
namespace {
void explain_rec(const Operator* op, int depth, std::ostringstream& os) {
    for (int i = 0; i < depth; ++i) os << "  ";
    os << "-> " << op->label() << "\n";
    for (const Operator* c : op->children()) explain_rec(c, depth + 1, os);
}
}  // namespace

std::string explain_tree(const Operator* root) {
    std::ostringstream os;
    explain_rec(root, 0, os);
    return os.str();
}

// ---------------------------------------------------------------------------
// SeqScan
// ---------------------------------------------------------------------------
SeqScan::SeqScan(ExecContext* ctx, TableHandle* table, const std::string& alias)
    : ctx_(ctx), table_(table), alias_(alias.empty() ? table->name : alias) {
    schema_ = schema_for(table, alias_);
}

void SeqScan::open() {
    it_ = std::make_unique<HeapFile::Iterator>(table_->heap->begin());
    end_ = std::make_unique<HeapFile::Iterator>(table_->heap->end());
}

bool SeqScan::next(ExecRow& out) {
    while (*it_ != *end_) {
        auto pr = **it_;
        ++(*it_);
        RID rid = pr.first;
        lock_shared(ctx_, table_->file_id, rid);
        out.values = table_->schema->deserialize(pr.second);
        out.sources = {RowSource{table_->file_id, rid}};
        return true;
    }
    return false;
}

void SeqScan::close() {
    it_.reset();
    end_.reset();
}

std::string SeqScan::label() const {
    std::ostringstream os;
    os << "SeqScan(" << table_->name;
    if (alias_ != table_->name) os << " " << alias_;
    os << ")  est_rows=" << table_->row_count();
    return os.str();
}

// ---------------------------------------------------------------------------
// IndexScan
// ---------------------------------------------------------------------------
IndexScan::IndexScan(ExecContext* ctx, TableHandle* table,
                     const std::string& alias, const IndexHandle* index,
                     const std::optional<Value>& lo, bool lo_inc,
                     const std::optional<Value>& hi, bool hi_inc,
                     const std::string& reason)
    : ctx_(ctx),
      table_(table),
      alias_(alias.empty() ? table->name : alias),
      index_(index),
      lo_(lo),
      hi_(hi),
      lo_inc_(lo_inc),
      hi_inc_(hi_inc),
      reason_(reason) {
    schema_ = schema_for(table, alias_);
}

void IndexScan::open() {
    matches_ = index_->tree->range(lo_, lo_inc_, hi_, hi_inc_);
    pos_ = 0;
}

bool IndexScan::next(ExecRow& out) {
    while (pos_ < matches_.size()) {
        RID rid = matches_[pos_++].second;
        std::vector<uint8_t> bytes;
        if (!table_->heap->get(rid, bytes)) continue;  // concurrently removed
        lock_shared(ctx_, table_->file_id, rid);
        out.values = table_->schema->deserialize(bytes);
        out.sources = {RowSource{table_->file_id, rid}};
        return true;
    }
    return false;
}

void IndexScan::close() { matches_.clear(); }

std::string IndexScan::label() const {
    std::ostringstream os;
    os << "IndexScan(" << table_->name << "." << index_->name << ")  "
       << reason_;
    return os.str();
}

// ---------------------------------------------------------------------------
// Filter
// ---------------------------------------------------------------------------
Filter::Filter(std::unique_ptr<Operator> child, std::vector<Predicate> preds)
    : child_(std::move(child)), preds_(std::move(preds)) {
    schema_ = child_->schema();
}
void Filter::open() { child_->open(); }
bool Filter::next(ExecRow& out) {
    ExecRow row;
    while (child_->next(row)) {
        bool ok = true;
        for (const auto& p : preds_) {
            if (!eval_predicate(p, row, schema_)) { ok = false; break; }
        }
        if (ok) { out = std::move(row); return true; }
    }
    return false;
}
void Filter::close() { child_->close(); }
std::string Filter::label() const {
    return "Filter(" + std::to_string(preds_.size()) + " predicate(s))";
}

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------
Project::Project(std::unique_ptr<Operator> child, std::vector<ColumnRef> cols)
    : child_(std::move(child)) {
    for (const auto& c : cols) indices_.push_back(resolve_in_schema(child_->schema(), c));
    schema_ = std::move(cols);
}
void Project::open() { child_->open(); }
bool Project::next(ExecRow& out) {
    ExecRow row;
    if (!child_->next(row)) return false;
    out.values.clear();
    for (int idx : indices_) out.values.push_back(row.values[idx]);
    out.sources = row.sources;
    return true;
}
void Project::close() { child_->close(); }
std::string Project::label() const {
    return "Project(" + std::to_string(indices_.size()) + " column(s))";
}

// ---------------------------------------------------------------------------
// NestedLoopJoin
// ---------------------------------------------------------------------------
NestedLoopJoin::NestedLoopJoin(std::unique_ptr<Operator> outer,
                               std::unique_ptr<Operator> inner, Predicate on)
    : outer_(std::move(outer)), inner_(std::move(inner)), on_(on) {
    schema_ = outer_->schema();
    for (const auto& c : inner_->schema()) schema_.push_back(c);
}
void NestedLoopJoin::open() {
    outer_->open();
    inner_->open();
    ExecRow r;
    while (inner_->next(r)) inner_rows_.push_back(r);  // materialise inner
    inner_->close();
    have_outer_ = false;
    inner_pos_ = 0;
}
bool NestedLoopJoin::next(ExecRow& out) {
    while (true) {
        if (!have_outer_) {
            if (!outer_->next(cur_outer_)) return false;
            have_outer_ = true;
            inner_pos_ = 0;
        }
        while (inner_pos_ < inner_rows_.size()) {
            const ExecRow& inner = inner_rows_[inner_pos_++];
            ExecRow combined = cur_outer_;
            combined.values.insert(combined.values.end(), inner.values.begin(),
                                   inner.values.end());
            combined.sources.insert(combined.sources.end(),
                                    inner.sources.begin(), inner.sources.end());
            if (eval_predicate(on_, combined, schema_)) {
                out = std::move(combined);
                return true;
            }
        }
        have_outer_ = false;
    }
}
void NestedLoopJoin::close() { outer_->close(); }
std::string NestedLoopJoin::label() const { return "NestedLoopJoin"; }

// ---------------------------------------------------------------------------
// IndexNestedLoopJoin
// ---------------------------------------------------------------------------
IndexNestedLoopJoin::IndexNestedLoopJoin(ExecContext* ctx,
                                         std::unique_ptr<Operator> outer,
                                         TableHandle* inner_table,
                                         const std::string& inner_alias,
                                         const IndexHandle* inner_index,
                                         Predicate on)
    : ctx_(ctx),
      outer_(std::move(outer)),
      inner_table_(inner_table),
      inner_alias_(inner_alias.empty() ? inner_table->name : inner_alias),
      inner_index_(inner_index),
      on_(on) {
    inner_schema_ = schema_for(inner_table, inner_alias_);
    schema_ = outer_->schema();
    for (const auto& c : inner_schema_) schema_.push_back(c);

    // Work out which side of the equality is the outer key.
    int li = -1;
    try { li = resolve_in_schema(outer_->schema(), on_.left); } catch (...) {}
    if (li != -1) {
        outer_key_idx_ = li;
    } else {
        outer_key_idx_ = resolve_in_schema(outer_->schema(), on_.right_col);
    }
}
void IndexNestedLoopJoin::open() {
    outer_->open();
    inner_pos_ = 0;
    inner_matches_.clear();
}
bool IndexNestedLoopJoin::advance_outer() {
    while (outer_->next(cur_outer_)) {
        const Value& key = cur_outer_.values[outer_key_idx_];
        inner_matches_ = inner_index_->tree->search(key);
        inner_pos_ = 0;
        return true;
    }
    return false;
}
bool IndexNestedLoopJoin::next(ExecRow& out) {
    while (true) {
        if (inner_pos_ < inner_matches_.size()) {
            RID rid = inner_matches_[inner_pos_++];
            std::vector<uint8_t> bytes;
            if (!inner_table_->heap->get(rid, bytes)) continue;
            lock_shared(ctx_, inner_table_->file_id, rid);
            ExecRow combined = cur_outer_;
            Tuple inner = inner_table_->schema->deserialize(bytes);
            combined.values.insert(combined.values.end(), inner.begin(),
                                   inner.end());
            combined.sources.push_back(RowSource{inner_table_->file_id, rid});
            out = std::move(combined);
            return true;
        }
        if (!advance_outer()) return false;
    }
}
void IndexNestedLoopJoin::close() { outer_->close(); }
std::string IndexNestedLoopJoin::label() const {
    return "IndexNestedLoopJoin(probe " + inner_table_->name + "." +
           inner_index_->name + ")";
}

// ---------------------------------------------------------------------------
// Statement executors
// ---------------------------------------------------------------------------
SelectResult run_select(Operator* root) {
    SelectResult result;
    for (const auto& ref : root->schema()) {
        result.columns.push_back(ref.table.empty() ? ref.col
                                                    : ref.table + "." + ref.col);
    }
    root->open();
    ExecRow row;
    while (root->next(row)) result.rows.push_back(row.values);
    root->close();
    return result;
}

int run_insert(ExecContext* ctx, TableHandle* table, const InsertStmt& stmt) {
    const Schema& schema = *table->schema;
    txn_id_t tid = ctx->txn ? ctx->txn->id() : INVALID_TXN_ID;

    // Build a per-row mapping from VALUES position -> schema column index.
    std::vector<int> col_order;  // schema index for each provided value column
    if (stmt.columns.empty()) {
        for (std::size_t i = 0; i < schema.num_columns(); ++i)
            col_order.push_back(static_cast<int>(i));
    } else {
        for (const auto& name : stmt.columns) {
            int ci = schema.column_index(name);
            if (ci < 0) throw SQLException("INSERT: unknown column '" + name + "'");
            col_order.push_back(ci);
        }
    }

    int count = 0;
    for (const auto& vals : stmt.rows) {
        if (vals.size() != col_order.size())
            throw SQLException("INSERT: value count does not match column count");
        // Every column must be supplied (MiniDB has no NULL / defaults).
        if (col_order.size() != schema.num_columns())
            throw SQLException("INSERT must provide a value for every column");

        Tuple tuple(schema.num_columns());
        for (std::size_t i = 0; i < vals.size(); ++i) tuple[col_order[i]] = vals[i];

        // Primary-key uniqueness check.
        int pk = schema.primary_key_index();
        for (const auto& idx : table->indexes) {
            if (idx.primary && !idx.tree->search(tuple[pk]).empty())
                throw ConstraintException("duplicate primary key '" +
                                          tuple[pk].to_string() + "'");
        }

        std::vector<uint8_t> bytes = schema.serialize(tuple);
        RID rid = table->heap->insert(bytes, tid);  // logs INSERT
        lock_exclusive(ctx, table->file_id, rid);
        for (const auto& idx : table->indexes)
            idx.tree->insert(tuple[idx.column], rid);
        if (ctx->txn)
            ctx->txn->record_undo({true, table->file_id, rid, {}});
        ++count;
    }
    return count;
}

int run_delete(ExecContext* ctx, TableHandle* table,
               const std::vector<Predicate>& where) {
    // Scan (and filter) first, collecting victims, before mutating anything --
    // this avoids invalidating the scan's heap iterator mid-delete.
    std::unique_ptr<Operator> scan =
        std::make_unique<SeqScan>(ctx, table, "");
    std::unique_ptr<Operator> plan;
    if (where.empty())
        plan = std::move(scan);
    else
        plan = std::make_unique<Filter>(std::move(scan), where);

    struct Victim { RID rid; Tuple tuple; };
    std::vector<Victim> victims;
    plan->open();
    ExecRow row;
    while (plan->next(row)) {
        victims.push_back({row.sources[0].rid, row.values});
    }
    plan->close();

    txn_id_t tid = ctx->txn ? ctx->txn->id() : INVALID_TXN_ID;
    int count = 0;
    for (const auto& v : victims) {
        lock_exclusive(ctx, table->file_id, v.rid);
        std::vector<uint8_t> before;
        if (!table->heap->get(v.rid, before)) continue;  // already gone
        table->heap->remove(v.rid, tid);  // logs DELETE (before-image)
        for (const auto& idx : table->indexes)
            idx.tree->erase(v.tuple[idx.column], v.rid);
        if (ctx->txn)
            ctx->txn->record_undo({false, table->file_id, v.rid, before});
        ++count;
    }
    return count;
}

}  // namespace minidb
