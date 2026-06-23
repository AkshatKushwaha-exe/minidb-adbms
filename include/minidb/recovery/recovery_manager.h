// Crash recovery.
//
// We implement a simplified version of ARIES that "repeats history": after a
// crash we replay the entire log forward (redo), then roll back the changes of
// any transaction that did not commit (undo). It runs in three conceptual
// phases:
//
//   1. Analysis -- scan the log to find which transactions committed.
//   2. Redo     -- re-apply every logged INSERT/DELETE to the heap pages,
//                  skipping a record if the page already reflects it (the
//                  page-LSN guard makes redo idempotent).
//   3. Undo     -- for every transaction that did NOT commit, apply the inverse
//                  of each of its operations in reverse order (an INSERT is
//                  undone by deleting, a DELETE by re-inserting its old bytes).
//
// The net effect: committed transactions are preserved and uncommitted ones
// leave no trace -- exactly the durability + atomicity guarantee we want.
#pragma once

#include <string>
#include <vector>

#include "minidb/recovery/log_record.h"
#include "minidb/storage/buffer_pool.h"

namespace minidb {

struct RecoveryStats {
    int committed_txns = 0;
    int loser_txns = 0;
    int redone = 0;
    int undone = 0;
};

class RecoveryManager {
public:
    RecoveryManager(BufferPool* bpool, const std::string& wal_path)
        : bpool_(bpool), wal_path_(wal_path) {}

    // Run recovery against the WAL. The engine must already have registered
    // every table's file with the buffer pool (using the stable catalog ids).
    RecoveryStats recover();

private:
    void ensure_page_exists(int file_id, page_id_t page_id);
    // Redo one record if the page does not already reflect it.
    void redo_record(const LogRecord& r);
    // Undo one record (apply its inverse).
    void undo_record(const LogRecord& r);

    BufferPool* bpool_;
    std::string wal_path_;
};

}  // namespace minidb
