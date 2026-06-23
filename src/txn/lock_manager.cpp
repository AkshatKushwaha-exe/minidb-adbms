#include "minidb/txn/lock_manager.h"

#include <algorithm>
#include <string>

#include "minidb/exceptions.h"

namespace minidb {

bool LockManager::can_grant(txn_id_t txn, const Resource& res,
                            LockMode mode) const {
    auto it = table_.find(res);
    if (it == table_.end() || it->second.empty()) return true;
    for (const Holder& h : it->second) {
        if (h.txn == txn) continue;  // our own lock never blocks our upgrade
        // Some *other* transaction holds a lock.
        if (mode == LockMode::EXCLUSIVE) return false;  // X needs exclusivity
        if (h.mode == LockMode::EXCLUSIVE) return false; // S blocked by an X
    }
    return true;
}

bool LockManager::has_deadlock(txn_id_t start) const {
    // DFS the wait-for graph looking for a cycle that returns to `start`.
    std::unordered_set<txn_id_t> visited;
    std::vector<txn_id_t> stack;
    auto edges = [&](txn_id_t u) -> const std::unordered_set<txn_id_t>* {
        auto it = waits_for_.find(u);
        return it == waits_for_.end() ? nullptr : &it->second;
    };
    const auto* start_edges = edges(start);
    if (!start_edges) return false;
    for (txn_id_t v : *start_edges) stack.push_back(v);
    while (!stack.empty()) {
        txn_id_t u = stack.back();
        stack.pop_back();
        if (u == start) return true;       // cycle back to the requester
        if (!visited.insert(u).second) continue;
        if (const auto* e = edges(u)) {
            for (txn_id_t v : *e) stack.push_back(v);
        }
    }
    return false;
}

bool LockManager::acquire(Transaction* txn, const Resource& res,
                          LockMode mode) {
    std::unique_lock<std::mutex> lk(latch_);
    txn_id_t id = txn->id();

    while (!can_grant(id, res, mode)) {
        // Record what we are waiting for and check for a deadlock cycle.
        std::unordered_set<txn_id_t> blockers;
        auto it = table_.find(res);
        if (it != table_.end()) {
            for (const Holder& h : it->second) {
                if (h.txn != id) blockers.insert(h.txn);
            }
        }
        waits_for_[id] = blockers;
        if (has_deadlock(id)) {
            waits_for_.erase(id);
            txn->set_state(TxnState::ABORTED);
            throw DeadlockException("deadlock detected; aborting transaction " +
                                    std::to_string(id));
        }
        cv_.wait(lk);  // released a lock somewhere; re-evaluate
    }
    waits_for_.erase(id);

    // Grant: upgrade an existing entry or add a new holder.
    auto& holders = table_[res];
    for (Holder& h : holders) {
        if (h.txn == id) {
            if (mode == LockMode::EXCLUSIVE) h.mode = LockMode::EXCLUSIVE;
            return true;  // already noted on first acquire
        }
    }
    holders.push_back({id, mode});
    txn->note_lock(res);
    return true;
}

bool LockManager::lock_shared(Transaction* txn, const Resource& res) {
    return acquire(txn, res, LockMode::SHARED);
}

bool LockManager::lock_exclusive(Transaction* txn, const Resource& res) {
    return acquire(txn, res, LockMode::EXCLUSIVE);
}

void LockManager::unlock_all(Transaction* txn) {
    std::unique_lock<std::mutex> lk(latch_);
    txn_id_t id = txn->id();
    for (const Resource& res : txn->locks()) {
        auto it = table_.find(res);
        if (it == table_.end()) continue;
        auto& holders = it->second;
        holders.erase(std::remove_if(holders.begin(), holders.end(),
                                     [&](const Holder& h) { return h.txn == id; }),
                      holders.end());
        if (holders.empty()) table_.erase(it);
    }
    // Drop this txn from the wait-for graph entirely (no stale edges).
    waits_for_.erase(id);
    for (auto& kv : waits_for_) kv.second.erase(id);
    cv_.notify_all();  // wake everyone to re-check their lock requests
}

}  // namespace minidb
