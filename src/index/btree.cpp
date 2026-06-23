#include "minidb/index/btree.h"

#include <algorithm>

namespace minidb {

BTree::BTree(int order) : order_(order < 3 ? 3 : order) {
    // A non-root node must keep at least this many keys. Chosen so that merging
    // two minimum nodes (+ a separator for internal nodes) never overflows.
    min_keys_ = (order_ - 1) / 2;
    if (min_keys_ < 1) min_keys_ = 1;
    root_ = std::make_unique<Node>(/*leaf=*/true);
}

// Index of the child to descend into for `key`. Equal keys go to the right
// subtree, so we use the position of the first separator strictly greater.
int BTree::child_index(const Node* n, const Value& key) const {
    auto it = std::upper_bound(n->keys.begin(), n->keys.end(), key);
    return static_cast<int>(it - n->keys.begin());
}

// --- search -----------------------------------------------------------------
std::vector<RID> BTree::search(const Value& key) const {
    const Node* n = root_.get();
    while (!n->leaf) {
        n = n->children[child_index(n, key)].get();
    }
    auto it = std::lower_bound(n->keys.begin(), n->keys.end(), key);
    if (it != n->keys.end() && *it == key) {
        return n->rids[it - n->keys.begin()];
    }
    return {};
}

// --- insert -----------------------------------------------------------------
void BTree::insert(const Value& key, const RID& rid) {
    auto split = insert_rec(root_.get(), key, rid);
    if (split) {
        // Root split: build a new root one level taller.
        auto new_root = std::make_unique<Node>(/*leaf=*/false);
        new_root->keys.push_back(split->sep);
        new_root->children.push_back(std::move(root_));
        new_root->children.push_back(std::move(split->right));
        root_ = std::move(new_root);
    }
}

std::optional<BTree::SplitResult> BTree::insert_rec(Node* n, const Value& key,
                                                    const RID& rid) {
    if (n->leaf) {
        auto it = std::lower_bound(n->keys.begin(), n->keys.end(), key);
        int idx = static_cast<int>(it - n->keys.begin());
        if (it != n->keys.end() && *it == key) {
            n->rids[idx].push_back(rid);  // duplicate key
        } else {
            n->keys.insert(it, key);
            n->rids.insert(n->rids.begin() + idx, std::vector<RID>{rid});
        }
        ++count_;
        if (static_cast<int>(n->keys.size()) <= order_) return std::nullopt;
        return split_leaf(n);
    }

    int ci = child_index(n, key);
    auto child_split = insert_rec(n->children[ci].get(), key, rid);
    if (!child_split) return std::nullopt;
    // Insert the promoted separator and new right child into this node.
    n->keys.insert(n->keys.begin() + ci, child_split->sep);
    n->children.insert(n->children.begin() + ci + 1,
                       std::move(child_split->right));
    if (static_cast<int>(n->keys.size()) <= order_) return std::nullopt;
    return split_internal(n);
}

BTree::SplitResult BTree::split_leaf(Node* n) {
    int mid = static_cast<int>(n->keys.size()) / 2;
    auto right = std::make_unique<Node>(/*leaf=*/true);
    right->keys.assign(n->keys.begin() + mid, n->keys.end());
    right->rids.assign(n->rids.begin() + mid, n->rids.end());
    n->keys.erase(n->keys.begin() + mid, n->keys.end());
    n->rids.erase(n->rids.begin() + mid, n->rids.end());
    // Maintain the leaf chain.
    right->next = n->next;
    n->next = right.get();
    SplitResult res;
    res.sep = right->keys.front();  // smallest key of the right leaf
    res.right = std::move(right);
    return res;
}

BTree::SplitResult BTree::split_internal(Node* n) {
    int mid = static_cast<int>(n->keys.size()) / 2;
    Value promoted = n->keys[mid];
    auto right = std::make_unique<Node>(/*leaf=*/false);
    // Right gets keys after mid and the corresponding children.
    right->keys.assign(n->keys.begin() + mid + 1, n->keys.end());
    right->children.assign(
        std::make_move_iterator(n->children.begin() + mid + 1),
        std::make_move_iterator(n->children.end()));
    // Left keeps keys before mid and children up to mid.
    n->keys.erase(n->keys.begin() + mid, n->keys.end());
    n->children.erase(n->children.begin() + mid + 1, n->children.end());
    SplitResult res;
    res.sep = promoted;
    res.right = std::move(right);
    return res;
}

// --- erase ------------------------------------------------------------------
bool BTree::erase(const Value& key) {
    bool removed = false;
    erase_rec(root_.get(), key, nullptr, removed);
    if (!root_->leaf && root_->keys.empty()) {
        root_ = std::move(root_->children[0]);  // shrink height
    }
    return removed;
}

bool BTree::erase(const Value& key, const RID& rid) {
    bool removed = false;
    erase_rec(root_.get(), key, &rid, removed);
    if (!root_->leaf && root_->keys.empty()) {
        root_ = std::move(root_->children[0]);
    }
    return removed;
}

void BTree::erase_rec(Node* n, const Value& key, const RID* rid,
                      bool& removed) {
    if (n->leaf) {
        auto it = std::lower_bound(n->keys.begin(), n->keys.end(), key);
        if (it == n->keys.end() || !(*it == key)) return;  // key not present
        int idx = static_cast<int>(it - n->keys.begin());
        if (rid != nullptr) {
            auto& list = n->rids[idx];
            auto rit = std::find(list.begin(), list.end(), *rid);
            if (rit == list.end()) return;  // that exact mapping is absent
            list.erase(rit);
            --count_;
            removed = true;
            if (!list.empty()) return;  // key still has other rids; keep it
        } else {
            count_ -= n->rids[idx].size();
            removed = true;
        }
        n->keys.erase(n->keys.begin() + idx);
        n->rids.erase(n->rids.begin() + idx);
        return;
    }

    int ci = child_index(n, key);
    erase_rec(n->children[ci].get(), key, rid, removed);
    if (removed) rebalance_child(n, ci);
}

void BTree::rebalance_child(Node* parent, int ci) {
    Node* child = parent->children[ci].get();
    if (static_cast<int>(child->keys.size()) >= min_keys_) return;  // fine

    int last = static_cast<int>(parent->children.size()) - 1;
    // Prefer borrowing (cheaper than merging) from whichever sibling can spare.
    if (ci > 0 &&
        static_cast<int>(parent->children[ci - 1]->keys.size()) > min_keys_) {
        borrow_from_left(parent, ci);
    } else if (ci < last &&
               static_cast<int>(parent->children[ci + 1]->keys.size()) >
                   min_keys_) {
        borrow_from_right(parent, ci);
    } else if (ci > 0) {
        merge(parent, ci - 1);  // merge child into its left sibling
    } else {
        merge(parent, ci);  // merge right sibling into child
    }
}

void BTree::borrow_from_left(Node* parent, int ci) {
    Node* child = parent->children[ci].get();
    Node* left = parent->children[ci - 1].get();
    if (child->leaf) {
        // Move the left sibling's largest key/rid to the front of child.
        child->keys.insert(child->keys.begin(), left->keys.back());
        child->rids.insert(child->rids.begin(), left->rids.back());
        left->keys.pop_back();
        left->rids.pop_back();
        parent->keys[ci - 1] = child->keys.front();  // new separator
    } else {
        // Rotate through the parent: parent sep down, left's last key up.
        child->keys.insert(child->keys.begin(), parent->keys[ci - 1]);
        child->children.insert(child->children.begin(),
                               std::move(left->children.back()));
        left->children.pop_back();
        parent->keys[ci - 1] = left->keys.back();
        left->keys.pop_back();
    }
}

void BTree::borrow_from_right(Node* parent, int ci) {
    Node* child = parent->children[ci].get();
    Node* right = parent->children[ci + 1].get();
    if (child->leaf) {
        child->keys.push_back(right->keys.front());
        child->rids.push_back(right->rids.front());
        right->keys.erase(right->keys.begin());
        right->rids.erase(right->rids.begin());
        parent->keys[ci] = right->keys.front();  // new separator
    } else {
        child->keys.push_back(parent->keys[ci]);
        child->children.push_back(std::move(right->children.front()));
        right->children.erase(right->children.begin());
        parent->keys[ci] = right->keys.front();
        right->keys.erase(right->keys.begin());
    }
}

void BTree::merge(Node* parent, int left_idx) {
    Node* left = parent->children[left_idx].get();
    Node* right = parent->children[left_idx + 1].get();
    if (left->leaf) {
        // Concatenate the two leaves; drop the separator.
        left->keys.insert(left->keys.end(), right->keys.begin(),
                          right->keys.end());
        left->rids.insert(left->rids.end(), right->rids.begin(),
                          right->rids.end());
        left->next = right->next;  // fix the leaf chain
    } else {
        // Pull the separator down, then append the right node's contents.
        left->keys.push_back(parent->keys[left_idx]);
        left->keys.insert(left->keys.end(), right->keys.begin(),
                          right->keys.end());
        for (auto& c : right->children) {
            left->children.push_back(std::move(c));
        }
    }
    parent->keys.erase(parent->keys.begin() + left_idx);
    parent->children.erase(parent->children.begin() + left_idx + 1);
}

// --- range scan -------------------------------------------------------------
const BTree::Node* BTree::leftmost_leaf() const {
    const Node* n = root_.get();
    while (!n->leaf) n = n->children[0].get();
    return n;
}

std::vector<std::pair<Value, RID>> BTree::range(
    const std::optional<Value>& lo, bool lo_inclusive,
    const std::optional<Value>& hi, bool hi_inclusive) const {
    std::vector<std::pair<Value, RID>> out;

    // Descend to the leaf where the lower bound would live, then walk the leaf
    // chain collecting in-range keys.
    const Node* n = root_.get();
    if (lo.has_value()) {
        while (!n->leaf) n = n->children[child_index(n, *lo)].get();
    } else {
        n = leftmost_leaf();
    }

    while (n != nullptr) {
        for (std::size_t i = 0; i < n->keys.size(); ++i) {
            const Value& k = n->keys[i];
            if (lo.has_value()) {
                if (lo_inclusive ? (k < *lo) : (k <= *lo)) continue;
            }
            if (hi.has_value()) {
                if (hi_inclusive ? (k > *hi) : (k >= *hi)) {
                    return out;  // past the upper bound: done
                }
            }
            for (const RID& r : n->rids[i]) out.emplace_back(k, r);
        }
        n = n->next;
    }
    return out;
}

// --- introspection ----------------------------------------------------------
int BTree::height() const {
    int h = 0;
    const Node* n = root_.get();
    if (n->keys.empty() && n->leaf) return 0;
    while (n != nullptr) {
        ++h;
        if (n->leaf) break;
        n = n->children[0].get();
    }
    return h;
}

bool BTree::validate() const {
    int leaf_depth = -1;
    return validate_rec(root_.get(), leaf_depth, 0);
}

bool BTree::validate_rec(const Node* n, int& leaf_depth, int depth) const {
    // Keys must be sorted within a node.
    for (std::size_t i = 1; i < n->keys.size(); ++i) {
        if (!(n->keys[i - 1] < n->keys[i])) return false;
    }
    if (n->leaf) {
        if (n->keys.size() != n->rids.size()) return false;
        if (leaf_depth == -1) leaf_depth = depth;
        return leaf_depth == depth;  // all leaves at same depth
    }
    if (n->children.size() != n->keys.size() + 1) return false;
    for (const auto& c : n->children) {
        if (!validate_rec(c.get(), leaf_depth, depth + 1)) return false;
    }
    return true;
}

}  // namespace minidb
