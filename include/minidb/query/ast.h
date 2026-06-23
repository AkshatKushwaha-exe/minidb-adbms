// Abstract syntax tree for the SQL subset MiniDB understands.
//
// We deliberately support a small, well-defined grammar:
//   CREATE TABLE t (col INT/TEXT [PRIMARY KEY], ...);
//   CREATE INDEX name ON t (col);
//   INSERT INTO t [(cols)] VALUES (..), (..);
//   SELECT a, b / *  FROM t [alias] [JOIN u [alias] ON a.x = u.y]... [WHERE ...];
//   DELETE FROM t [WHERE ...];
//   BEGIN; COMMIT; ABORT;  (also ROLLBACK as a synonym for ABORT)
//
// WHERE is a conjunction (AND) of simple comparisons: a column compared with a
// literal, or a column compared with another column (used for join conditions).
#pragma once

#include <string>
#include <vector>

#include "minidb/record/value.h"

namespace minidb {

enum class StmtType {
    CREATE_TABLE,
    CREATE_INDEX,
    INSERT,
    SELECT,
    DELETE,
    BEGIN,
    COMMIT,
    ABORT,
};

enum class CompOp { EQ, NE, LT, LE, GT, GE };

// A reference to a column, optionally qualified by a table/alias (e.g. "u.id").
struct ColumnRef {
    std::string table;  // empty if unqualified
    std::string col;
};

// One comparison in a WHERE/ON clause.
struct Predicate {
    ColumnRef left;
    CompOp op;
    bool right_is_column = false;
    ColumnRef right_col;   // valid if right_is_column
    Value right_value;     // valid otherwise (a literal)
};

struct ColumnDef {
    std::string name;
    Type type;
    bool primary_key = false;
};

struct CreateTableStmt {
    std::string table;
    std::vector<ColumnDef> columns;
};

struct CreateIndexStmt {
    std::string index_name;
    std::string table;
    std::string column;
};

struct InsertStmt {
    std::string table;
    std::vector<std::string> columns;       // empty => all columns, in order
    std::vector<std::vector<Value>> rows;    // one or more VALUES tuples
};

struct JoinClause {
    std::string table;
    std::string alias;   // empty if none
    Predicate on;        // join condition (typically an equality of columns)
};

struct SelectStmt {
    bool select_star = false;
    std::vector<ColumnRef> columns;  // valid if !select_star

    std::string from_table;
    std::string from_alias;          // empty if none
    std::vector<JoinClause> joins;
    std::vector<Predicate> where;    // ANDed together (empty => no filter)
};

struct DeleteStmt {
    std::string table;
    std::vector<Predicate> where;
};

// A parsed statement. Only the member matching `type` is meaningful.
struct Statement {
    StmtType type;
    CreateTableStmt create_table;
    CreateIndexStmt create_index;
    InsertStmt insert;
    SelectStmt select;
    DeleteStmt del;
};

}  // namespace minidb
