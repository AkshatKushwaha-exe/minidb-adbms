// An in-memory B+ tree index mapping a key (Value) to one or more RIDs.
//
// Why in-memory? We treat indexes as *derived* structures: the engine rebuilds
// each tree by scanning the heap file at startup. This keeps crash recovery
// focused on the heap (the source of truth) and means the index is always
// consistent with the data after recovery. The trade-off -- rebuild cost at
// open and memory usage -- is documented in docs/design-decisions.md.
//
// The tree supports:
//   * search(key)            -> the RIDs stored under an exact key
//   * range(lo, hi)          -> all (key, RID) pairs in a key range (ordered),
//                               which is what powers index range scans
//   * insert(key, rid)       -> add a mapping (duplicate keys allowed)
//   * erase(key)/erase(k,r)  -> remove a key (or one specific mapping)
//
// Insert splits full nodes; erase rebalances underfull nodes by borrowing from
// a sibling or merging -- i.e. a real, self-balancing B+ tree.
#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "minidb/constants.h"
#include "minidb/record/value.h"
#include "minidb/rid.h"

namespace minidb {

class BTree {
public:
    // `order` is the maximum number of keys a node may hold before it splits.
    explicit BTree(int order = BTREE_ORDER);

    // Insert a (key -> rid) mapping. Duplicate keys are allowed (the rid is
    // appended to the key's list); the caller enforces uniqueness if required.
    void insert(const Value& key, const RID& rid);

    // Remove every mapping for `key`. Returns true if the key existed.
    bool erase(const Value& key);

    // Remove the single mapping (key -> rid). Returns true if it existed. If
    // that was the key's last rid, the key itself is removed.
    bool erase(const Value& key, const RID& rid);

    // Return the RIDs stored under an exact key (empty if absent).
    std::vector<RID> search(const Value& key) const;

    // Return all (key, rid) pairs whose key lies within the optional bounds,
    // in ascending key order. A missing bound means unbounded on that side.
    std::vector<std::pair<Value, RID>> range(
        const std::optional<Value>& lo, bool lo_inclusive,
        const std::optional<Value>& hi, bool hi_inclusive) const;

    // Total number of (key, rid) mappings stored.
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    // Height of the tree (number of levels); 0 for an empty tree. Used by the
    // optimizer's cost model and by tests.
    int height() const;

    // Debug/test helper: verify all B+ tree invariants hold. Returns true if OK.
    bool validate() const;

private:
    struct Node {
        bool leaf;
        std::vector<Value> keys;
        // Internal nodes only: children.size() == keys.size() + 1.
        std::vector<std::unique_ptr<Node>> children;
        // Leaf nodes only: rids.size() == keys.size().
        std::vector<std::vector<RID>> rids;
        Node* next = nullptr;  // leaf-chain pointer (not owning)
        explicit Node(bool is_leaf) : leaf(is_leaf) {}
    };

    struct SplitResult {
        Value sep;                   // separator key promoted to the parent
        std::unique_ptr<Node> right; // newly created right sibling
    };

    int child_index(const Node* n, const Value& key) const;
    std::optional<SplitResult> insert_rec(Node* n, const Value& key,
                                          const RID& rid);
    SplitResult split_leaf(Node* n);
    SplitResult split_internal(Node* n);

    void erase_rec(Node* n, const Value& key, const RID* rid, bool& removed);
    void rebalance_child(Node* parent, int ci);
    void borrow_from_left(Node* parent, int ci);
    void borrow_from_right(Node* parent, int ci);
    void merge(Node* parent, int left_idx);  // merges child left_idx & left_idx+1

    const Node* leftmost_leaf() const;
    bool validate_rec(const Node* n, int& leaf_depth, int depth) const;

    std::unique_ptr<Node> root_;
    int order_;       // max keys per node
    int min_keys_;    // min keys for a non-root node
    std::size_t count_ = 0;
};

}  // namespace minidb
