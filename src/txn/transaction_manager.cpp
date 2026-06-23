#include "minidb/txn/transaction_manager.h"

namespace minidb {

Transaction* TransactionManager::begin() {
    txn_id_t id = next_id_++;
    auto txn = std::make_unique<Transaction>(id);
    Transaction* ptr = txn.get();
    {
        std::lock_guard<std::mutex> g(map_latch_);
        active_[id] = std::move(txn);
    }
    wal_->log_begin(id);
    return ptr;
}

void TransactionManager::commit(Transaction* txn) {
    wal_->log_commit(txn->id());  // forces the log to disk before we proceed
    lm_->unlock_all(txn);
    txn->set_state(TxnState::COMMITTED);
    std::lock_guard<std::mutex> g(map_latch_);
    active_.erase(txn->id());
}

void TransactionManager::abort(Transaction* txn) {
    // Roll back this transaction's changes in reverse order.
    const auto& undo = txn->undo_actions();
    if (undo_applier_) {
        for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
            undo_applier_(*it);
        }
    }
    wal_->log_abort(txn->id());
    lm_->unlock_all(txn);
    txn->set_state(TxnState::ABORTED);
    std::lock_guard<std::mutex> g(map_latch_);
    active_.erase(txn->id());
}

}  // namespace minidb
