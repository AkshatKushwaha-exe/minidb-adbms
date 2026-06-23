// Tests for the B+ tree, including a randomised comparison against a std::map
// "oracle" to flush out structural bugs in split / borrow / merge.
#include <algorithm>
#include <map>
#include <random>
#include <vector>

#include "minidb/index/btree.h"
#include "test_framework.h"

using namespace minidb;

static Value K(int64_t k) { return Value::make_int(k); }
static RID R(int p, int s) { return RID{p, s}; }

TEST(btree, basic_insert_search) {
    BTree t(4);
    for (int i = 0; i < 20; ++i) t.insert(K(i), R(i, 0));
    CHECK(t.validate());
    CHECK_EQ(t.size(), (size_t)20);
    for (int i = 0; i < 20; ++i) {
        auto rids = t.search(K(i));
        CHECK_EQ(rids.size(), (size_t)1);
        CHECK(rids[0] == R(i, 0));
    }
    CHECK(t.search(K(999)).empty());
    CHECK(t.height() >= 2);  // 20 keys with order 4 must be multi-level
}

TEST(btree, reverse_and_random_insert) {
    BTree t(4);
    std::vector<int> ks;
    for (int i = 0; i < 50; ++i) ks.push_back(i);
    std::mt19937 rng(12345);
    std::shuffle(ks.begin(), ks.end(), rng);
    for (int k : ks) t.insert(K(k), R(k, 1));
    CHECK(t.validate());
    for (int i = 0; i < 50; ++i) {
        auto r = t.search(K(i));
        CHECK_EQ(r.size(), (size_t)1);
    }
}

TEST(btree, duplicate_keys) {
    BTree t(4);
    t.insert(K(5), R(0, 0));
    t.insert(K(5), R(0, 1));
    t.insert(K(5), R(0, 2));
    auto r = t.search(K(5));
    CHECK_EQ(r.size(), (size_t)3);
    CHECK_EQ(t.size(), (size_t)3);
    // erase one specific rid, key remains
    CHECK(t.erase(K(5), R(0, 1)));
    CHECK_EQ(t.search(K(5)).size(), (size_t)2);
    // erase whole key
    CHECK(t.erase(K(5)));
    CHECK(t.search(K(5)).empty());
}

TEST(btree, range_scan) {
    BTree t(4);
    for (int i = 0; i < 30; ++i) t.insert(K(i * 2), R(i, 0));  // 0,2,4,...
    // [10, 20] inclusive -> 10,12,14,16,18,20 (6 keys)
    auto rows = t.range(K(10), true, K(20), true);
    CHECK_EQ(rows.size(), (size_t)6);
    CHECK(rows.front().first == K(10));
    CHECK(rows.back().first == K(20));
    // exclusive bounds (10,20) -> 12,14,16,18 (4)
    auto rows2 = t.range(K(10), false, K(20), false);
    CHECK_EQ(rows2.size(), (size_t)4);
    // unbounded below, < 6 -> 0,2,4 (3)
    auto rows3 = t.range(std::nullopt, true, K(6), false);
    CHECK_EQ(rows3.size(), (size_t)3);
    // full scan in order
    auto all = t.range(std::nullopt, true, std::nullopt, true);
    CHECK_EQ(all.size(), (size_t)30);
    for (size_t i = 1; i < all.size(); ++i) {
        CHECK(all[i - 1].first < all[i].first);
    }
}

TEST(btree, erase_with_rebalance) {
    BTree t(4);
    for (int i = 0; i < 40; ++i) t.insert(K(i), R(i, 0));
    CHECK(t.validate());
    // delete every key; tree must stay valid throughout and end empty
    for (int i = 0; i < 40; ++i) {
        CHECK(t.erase(K(i)));
        CHECK(t.validate());
    }
    CHECK_EQ(t.size(), (size_t)0);
    CHECK(t.empty());
    for (int i = 0; i < 40; ++i) CHECK(t.search(K(i)).empty());
}

TEST(btree, erase_descending_rebalance) {
    BTree t(5);
    for (int i = 0; i < 60; ++i) t.insert(K(i), R(i, 0));
    for (int i = 59; i >= 0; --i) {
        CHECK(t.erase(K(i)));
        CHECK(t.validate());
    }
    CHECK(t.empty());
}

// The important one: random insert/erase interleaving vs a std::map oracle.
TEST(btree, randomized_oracle) {
    BTree t(4);
    std::map<int64_t, std::vector<RID>> oracle;
    std::mt19937 rng(987654321);
    std::uniform_int_distribution<int> key_dist(0, 40);
    std::uniform_int_distribution<int> op_dist(0, 2);
    int slot_counter = 0;

    for (int step = 0; step < 4000; ++step) {
        int key = key_dist(rng);
        int op = op_dist(rng);
        if (op <= 1) {  // insert (twice as likely)
            RID rid = R(key, slot_counter++);
            t.insert(K(key), rid);
            oracle[key].push_back(rid);
        } else {  // erase whole key
            bool had = oracle.count(key) > 0;
            bool removed = t.erase(K(key));
            CHECK_EQ(removed, had);
            oracle.erase(key);
        }
        // Cheap invariant check most steps; full compare occasionally.
        if (step % 200 == 0) {
            CHECK(t.validate());
            size_t total = 0;
            for (auto& kv : oracle) total += kv.second.size();
            CHECK_EQ(t.size(), total);
        }
    }
    CHECK(t.validate());

    // Final exhaustive comparison of search results.
    for (int key = 0; key <= 40; ++key) {
        auto got = t.search(K(key));
        std::sort(got.begin(), got.end());
        std::vector<RID> want;
        if (oracle.count(key)) want = oracle[key];
        std::sort(want.begin(), want.end());
        CHECK_EQ(got.size(), want.size());
        CHECK(got == want);
    }

    // Range scan must match the oracle's ordered keys in [5, 30].
    auto rows = t.range(K(5), true, K(30), true);
    size_t expected = 0;
    for (auto& kv : oracle) {
        if (kv.first >= 5 && kv.first <= 30) expected += kv.second.size();
    }
    CHECK_EQ(rows.size(), expected);
    for (size_t i = 1; i < rows.size(); ++i) {
        CHECK(rows[i - 1].first <= rows[i].first);
    }
}

TEST(btree, text_keys) {
    BTree t(4);
    std::vector<std::string> names = {"delta", "alpha", "charlie",
                                      "echo",  "bravo", "foxtrot"};
    for (size_t i = 0; i < names.size(); ++i)
        t.insert(Value::make_text(names[i]), R((int)i, 0));
    CHECK(t.validate());
    auto r = t.search(Value::make_text("charlie"));
    CHECK_EQ(r.size(), (size_t)1);
    // range alpha..charlie inclusive -> alpha, bravo, charlie
    auto rows = t.range(Value::make_text("alpha"), true,
                        Value::make_text("charlie"), true);
    CHECK_EQ(rows.size(), (size_t)3);
}
