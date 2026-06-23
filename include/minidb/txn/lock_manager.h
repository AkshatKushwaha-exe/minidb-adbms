// The lock manager provides two-phase locking with deadlock detection, which
// together give serializable isolation.
//
// Granularity: one lock per record (Resource = file_id + RID). Modes are
// SHARED (for reads) and EXCLUSIVE (for writes). Compatibility is the usual
// matrix: two shared locks coexist; anything involving exclusive conflicts.
//
// Deadlock detection: when a transaction would have to wait, we add edges to a
// wait-for graph (waiter -> each transaction it is blocked on) and look for a
// cycle reachable from the waiter. If there is one, the waiter is chosen as the
// victim and a DeadlockException is thrown so it can abort and release its
// locks, letting the others proceed.
//
// Threading model: a single mutex guards the whole lock table and one condition
// variable wakes waiters whenever locks are released. This is simple and easy
// to reason about (the goal here is correctness and clarity, not raw scale).
#pragma once

#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "minidb/txn/transaction.h"

namespace minidb {

enum class LockMode { SHARED, EXCLUSIVE };

class LockManager {
public:
    // Acquire a shared / exclusive lock for `txn` on `res`. Blocks until the
    // lock is granted. Throws DeadlockException if granting would deadlock (the
    // caller must then abort `txn`). Returns true on success.
    bool lock_shared(Transaction* txn, const Resource& res);
    bool lock_exclusive(Transaction* txn, const Resource& res);

    // Release every lock held by `txn` (called at commit or abort). This is the
    // single point where locks are released -> strict two-phase locking.
    void unlock_all(Transaction* txn);

private:
    struct Holder {
        txn_id_t txn;
        LockMode mode;
    };

    bool acquire(Transaction* txn, const Resource& res, LockMode mode);
    // True if `txn` can hold `mode` on `res` given the current holders.
    bool can_grant(txn_id_t txn, const Resource& res, LockMode mode) const;
    // Is there a cycle in the wait-for graph reachable from `start`?
    bool has_deadlock(txn_id_t start) const;

    mutable std::mutex latch_;
    std::condition_variable cv_;

    // Currently granted locks per resource.
    std::unordered_map<Resource, std::vector<Holder>> table_;
    // Wait-for graph: txn -> set of txns it is currently waiting on.
    std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> waits_for_;
};

}  // namespace minidb
