// A small cost-based optimizer.
//
// It turns a parsed SELECT into a physical operator tree, making two kinds of
// decision that the lab brief asks for:
//
//   1. Access path  -- for each table, choose an index scan or a full table
//      scan based on whether a usable indexed predicate exists and how
//      selective it is (see selectivity estimation below).
//
//   2. Join order + algorithm -- order the relations smallest-first (greedy,
//      using row-count estimates) and, for each join, pick an index nested-loop
//      join when the inner relation's join column is indexed, otherwise a plain
//      (materialised) nested-loop join.
//
// The estimates are deliberately simple but real: equality on a unique/primary
// column is ~1 row, equality on a non-unique column ~10%, a range ~33%.
#pragma once

#include <memory>

#include "minidb/query/ast.h"
#include "minidb/query/executor.h"

namespace minidb {

class Optimizer {
public:
    // Build an executable operator tree for `stmt`. Throws on unknown tables /
    // columns. The returned tree's schema is the query's output schema.
    static std::unique_ptr<Operator> build_select(ExecContext* ctx,
                                                  const SelectStmt& stmt);

    // Exposed for tests: estimated selectivity of a predicate on a table.
    static double estimate_selectivity(const Predicate& p,
                                       const TableHandle* table);
};

}  // namespace minidb
