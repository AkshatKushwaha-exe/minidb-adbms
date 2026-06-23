// A Transaction tracks the state needed to provide atomicity + isolation:
//   * a unique id (also used to order transactions for deadlock victim choice)
//   * its current 2PL phase (growing = still acquiring locks, shrinking = has
//     started releasing -- in strict 2PL that only happens at commit/abort)
//   * an in-memory undo list so an abort can roll back its own changes
//   * the set of resources it has locked, so all locks are released at the end
#pragma once

#include <cstdint>
#include <vector>

#include "minidb/constants.h"
#include "minidb/rid.h"

namespace minidb {

enum class TxnState { GROWING, SHRINKING, COMMITTED, ABORTED };

// One reversible change. To undo an insert we delete the rid; to undo a delete
// we re-insert `image` at the rid.
struct UndoAction {
    bool was_insert;          // true: undo by deleting; false: undo by inserting
    int file_id;
    RID rid;
    std::vector<uint8_t> image;  // bytes to restore when undoing a delete
};

// Identifies a lockable resource: a specific record in a specific file.
struct Resource {
    int file_id;
    RID rid;
    bool operator==(const Resource& o) const {
        return file_id == o.file_id && rid == o.rid;
    }
};

class Transaction {
public:
    explicit Transaction(txn_id_t id) : id_(id), state_(TxnState::GROWING) {}

    txn_id_t id() const { return id_; }
    TxnState state() const { return state_; }
    void set_state(TxnState s) { state_ = s; }

    void record_undo(const UndoAction& a) { undo_.push_back(a); }
    // Undo actions in reverse order (most recent first) -- used during abort.
    const std::vector<UndoAction>& undo_actions() const { return undo_; }

    void note_lock(const Resource& r) { locks_.push_back(r); }
    const std::vector<Resource>& locks() const { return locks_; }

private:
    txn_id_t id_;
    TxnState state_;
    std::vector<UndoAction> undo_;
    std::vector<Resource> locks_;
};

}  // namespace minidb

namespace std {
template <>
struct hash<minidb::Resource> {
    size_t operator()(const minidb::Resource& r) const {
        return (std::hash<int>()(r.file_id) * 1000003u) ^
               std::hash<minidb::RID>()(r.rid);
    }
};
}  // namespace std
