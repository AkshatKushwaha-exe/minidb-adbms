// The catalog is the database's data dictionary: it remembers every table's
// schema and the indexes defined on it. It is persisted to a small text file so
// that table definitions survive a restart.
//
// Design note: the catalog stores index *definitions* only, not the B+ tree
// data itself. The engine rebuilds each B+ tree by scanning the (recovered)
// heap file at startup. Treating indexes as derived structures means we never
// have to log or recover them separately -- a deliberate simplification that
// keeps recovery focused on the heap files.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "minidb/record/schema.h"

namespace minidb {

struct IndexInfo {
    std::string name;
    int column;     // index of the column this index is built on
    bool unique;    // reject duplicate keys?
    bool primary;   // is this the primary-key index?
};

struct TableInfo {
    std::string name;
    Schema schema;
    std::vector<IndexInfo> indexes;
    // A stable id assigned at creation and persisted. The WAL records this id
    // (not a name) so that after a crash, recovery resolves every log record to
    // the correct heap file regardless of how many tables now exist.
    int id = 0;
};

class Catalog {
public:
    // Create a table. Throws CatalogException if it already exists. Also
    // creates the primary-key index entry automatically.
    void create_table(const std::string& name, const Schema& schema);

    // Register a secondary index on a table.
    void add_index(const std::string& table, const IndexInfo& index);

    bool has_table(const std::string& name) const;
    const TableInfo& get_table(const std::string& name) const;
    std::vector<std::string> table_names() const;

    // Persist / load the catalog to/from a text file.
    void save(const std::string& path) const;
    void load(const std::string& path);

    // Largest table id assigned so far + 1 (for sizing the engine's file table).
    int next_table_id() const { return next_id_; }

private:
    std::map<std::string, TableInfo> tables_;
    int next_id_ = 0;
};

}  // namespace minidb
