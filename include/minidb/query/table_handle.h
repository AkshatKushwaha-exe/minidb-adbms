// Runtime handles that the executor and optimizer use to reach a table's
// storage, indexes and statistics. The engine builds these and hands them out
// through ITableProvider, so the query layer never depends on the engine type
// directly (avoids a circular include).
#pragma once

#include <string>
#include <vector>

#include "minidb/index/btree.h"
#include "minidb/record/schema.h"
#include "minidb/storage/heap_file.h"

namespace minidb {

struct IndexHandle {
    std::string name;
    int column;     // which column of the table this index is on
    bool unique;
    bool primary;
    BTree* tree;    // owned by the engine
};

struct TableHandle {
    std::string name;
    int file_id;             // stable catalog id, used by the heap + WAL
    const Schema* schema;
    HeapFile* heap;          // owned by the engine
    std::vector<IndexHandle> indexes;

    // Estimated number of live rows. We use the primary index size, which is
    // an exact O(1) row count (every row has a primary-key entry).
    std::size_t row_count() const {
        for (const auto& idx : indexes)
            if (idx.primary) return idx.tree->size();
        return 0;
    }

    // The index built on `column`, or nullptr if there is none.
    const IndexHandle* index_on(int column) const {
        for (const auto& idx : indexes)
            if (idx.column == column) return &idx;
        return nullptr;
    }
};

// Lookup interface implemented by the engine.
class ITableProvider {
public:
    virtual ~ITableProvider() = default;
    virtual TableHandle* get_table(const std::string& name) = 0;
};

}  // namespace minidb
