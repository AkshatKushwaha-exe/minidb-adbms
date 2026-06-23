// The transaction manager hands out transactions and drives their lifecycle,
// tying together the WAL (for BEGIN/COMMIT/ABORT records) and the lock manager
// (for releasing locks under strict 2PL).
//
// On abort it rolls back the transaction's own changes by replaying its undo
// list in reverse through an "undo applier" callback supplied by the engine
// (so this module needs no direct dependency on the storage layer).
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "minidb/recovery/wal.h"
#include "minidb/txn/lock_manager.h"
#include "minidb/txn/transaction.h"

namespace minidb {

class TransactionManager {
public:
    using UndoApplier = std::function<void(const UndoAction&)>;

    TransactionManager(WAL* wal, LockManager* lm, UndoApplier undo_applier)
        : wal_(wal), lm_(lm), undo_applier_(std::move(undo_applier)) {}

    // Start a new transaction (logs BEGIN). The returned pointer is owned by the
    // manager and stays valid until commit()/abort() is called on it.
    Transaction* begin();

    // Commit: flush a COMMIT record (durability) and release all locks.
    void commit(Transaction* txn);

    // Abort: roll back the transaction's changes, log ABORT, release locks.
    void abort(Transaction* txn);

    // First id a future transaction would get -- used by the engine to seed the
    // counter from the WAL so ids never collide across restarts.
    void set_next_txn_id(txn_id_t id) { next_id_ = id; }

private:
    WAL* wal_;
    LockManager* lm_;
    UndoApplier undo_applier_;
    std::atomic<txn_id_t> next_id_{1};
    std::mutex map_latch_;
    std::map<txn_id_t, std::unique_ptr<Transaction>> active_;
};

}  // namespace minidb
