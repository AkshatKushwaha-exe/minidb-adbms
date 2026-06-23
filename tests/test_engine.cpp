// End-to-end tests that drive the whole engine through SQL: DDL, INSERT,
// SELECT (with WHERE/JOIN), DELETE, the optimizer's access-path choice,
// transactions, and durability across a reopen.
#include <set>
#include <string>

#include "minidb/engine.h"
#include "minidb/exceptions.h"
#include "test_framework.h"
#include "test_util.h"

using namespace minidb;

// Collect a single text column from a SELECT result for easy assertions.
static std::set<std::string> col_set(const SelectResult& r, int c) {
    std::set<std::string> out;
    for (const auto& row : r.rows) out.insert(row[c].to_string());
    return out;
}

TEST(engine, create_insert_select) {
    Engine db(minitest::temp_dir("e_basic"));
    db.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT)");
    db.execute("INSERT INTO users VALUES (1, 'alice', 30)");
    db.execute("INSERT INTO users VALUES (2, 'bob', 25), (3, 'carol', 40)");

    auto r = db.execute("SELECT id, name FROM users");
    CHECK(r.kind == QueryResult::Kind::SELECT);
    CHECK_EQ(r.select.columns.size(), (size_t)2);
    CHECK_EQ(r.select.rows.size(), (size_t)3);
    auto names = col_set(r.select, 1);
    CHECK(names.count("alice") == 1 && names.count("bob") == 1 &&
          names.count("carol") == 1);
}

TEST(engine, select_where_and_columns) {
    Engine db(minitest::temp_dir("e_where"));
    db.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT)");
    db.execute("INSERT INTO users VALUES (1,'alice',30),(2,'bob',25),(3,'carol',40)");

    auto r = db.execute("SELECT name FROM users WHERE age > 28");
    CHECK_EQ(r.select.rows.size(), (size_t)2);
    auto names = col_set(r.select, 0);
    CHECK(names.count("alice") == 1);
    CHECK(names.count("carol") == 1);
    CHECK(names.count("bob") == 0);
}

TEST(engine, primary_key_uniqueness) {
    Engine db(minitest::temp_dir("e_pk"));
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    db.execute("INSERT INTO t VALUES (1, 'a')");
    CHECK_THROWS(db.execute("INSERT INTO t VALUES (1, 'dup')"));
    auto r = db.execute("SELECT * FROM t");
    CHECK_EQ(r.select.rows.size(), (size_t)1);
}

TEST(engine, delete_rows) {
    Engine db(minitest::temp_dir("e_del"));
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    db.execute("INSERT INTO t VALUES (1,10),(2,20),(3,30),(4,40)");
    auto d = db.execute("DELETE FROM t WHERE v >= 30");
    CHECK_EQ(d.affected, 2);
    auto r = db.execute("SELECT id FROM t");
    CHECK_EQ(r.select.rows.size(), (size_t)2);
    auto ids = col_set(r.select, 0);
    CHECK(ids.count("1") == 1 && ids.count("2") == 1);
}

TEST(engine, optimizer_uses_index_for_pk_equality) {
    Engine db(minitest::temp_dir("e_opt"));
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    for (int i = 0; i < 100; ++i)
        db.execute("INSERT INTO t VALUES (" + std::to_string(i) + ", 'x')");

    // Equality on the primary key should pick an index scan...
    auto e1 = db.execute("EXPLAIN SELECT * FROM t WHERE id = 42");
    CHECK(e1.message.find("IndexScan") != std::string::npos);

    // ...but a predicate on a non-indexed column falls back to a seq scan.
    auto e2 = db.execute("EXPLAIN SELECT * FROM t WHERE v = 'x'");
    CHECK(e2.message.find("SeqScan") != std::string::npos);

    // And the index path returns the right row.
    auto r = db.execute("SELECT v FROM t WHERE id = 42");
    CHECK_EQ(r.select.rows.size(), (size_t)1);
}

TEST(engine, secondary_index_scan) {
    Engine db(minitest::temp_dir("e_sec"));
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, cat INT)");
    db.execute("CREATE INDEX idx_cat ON t (cat)");
    for (int i = 0; i < 50; ++i)
        db.execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                   std::to_string(i % 5) + ")");
    auto e = db.execute("EXPLAIN SELECT * FROM t WHERE cat = 3");
    CHECK(e.message.find("IndexScan") != std::string::npos);
    auto r = db.execute("SELECT id FROM t WHERE cat = 3");
    CHECK_EQ(r.select.rows.size(), (size_t)10);  // 3, 8, 13, ... 48
}

TEST(engine, join_query) {
    Engine db(minitest::temp_dir("e_join"));
    db.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT)");
    db.execute("CREATE TABLE orders (oid INT PRIMARY KEY, uid INT, item TEXT)");
    db.execute("INSERT INTO users VALUES (1,'alice'),(2,'bob')");
    db.execute("INSERT INTO orders VALUES (10,1,'book'),(11,1,'pen'),(12,2,'lamp')");

    auto r = db.execute(
        "SELECT u.name, o.item FROM users u JOIN orders o ON u.id = o.uid "
        "WHERE u.name = 'alice'");
    CHECK_EQ(r.select.rows.size(), (size_t)2);
    auto items = col_set(r.select, 1);
    CHECK(items.count("book") == 1);
    CHECK(items.count("pen") == 1);
    CHECK(items.count("lamp") == 0);
}

TEST(engine, transaction_commit_and_abort) {
    Engine db(minitest::temp_dir("e_txn"));
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");

    db.execute("BEGIN");
    db.execute("INSERT INTO t VALUES (1, 100)");
    db.execute("COMMIT");
    CHECK_EQ(db.execute("SELECT * FROM t").select.rows.size(), (size_t)1);

    db.execute("BEGIN");
    db.execute("INSERT INTO t VALUES (2, 200)");
    db.execute("ABORT");  // rolls back the insert
    auto r = db.execute("SELECT id FROM t");
    CHECK_EQ(r.select.rows.size(), (size_t)1);
    CHECK(col_set(r.select, 0).count("2") == 0);
}

TEST(engine, crash_recovery_preserves_committed) {
    std::string dir = minitest::temp_dir("e_crash");
    {
        Engine db(dir);
        db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
        // Two committed rows (auto-commit flushes the WAL on each commit).
        db.execute("INSERT INTO t VALUES (1,'committed'),(2,'also')");
        // An open transaction whose insert is never committed.
        db.execute("BEGIN");
        db.execute("INSERT INTO t VALUES (3,'lost')");
        db.simulate_crash();  // dirty pages are discarded on destruction
    }
    {
        Engine db(dir);  // reopen -> recovery rebuilds from the WAL
        auto r = db.execute("SELECT id, v FROM t");
        CHECK_EQ(r.select.rows.size(), (size_t)2);  // committed rows survive
        auto ids = col_set(r.select, 0);
        CHECK(ids.count("1") == 1);
        CHECK(ids.count("2") == 1);
        CHECK(ids.count("3") == 0);  // uncommitted insert rolled back
    }
}

TEST(engine, durability_across_reopen) {
    std::string dir = minitest::temp_dir("e_persist");
    {
        Engine db(dir);
        db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
        db.execute("INSERT INTO t VALUES (1,'one'),(2,'two')");
        // Engine destructor flushes + saves the catalog.
    }
    {
        Engine db(dir);  // reopens; recovery + index rebuild
        auto r = db.execute("SELECT v FROM t");
        CHECK_EQ(r.select.rows.size(), (size_t)2);
        auto vs = col_set(r.select, 0);
        CHECK(vs.count("one") == 1 && vs.count("two") == 1);
        // Index still works after rebuild.
        auto r2 = db.execute("SELECT v FROM t WHERE id = 2");
        CHECK_EQ(r2.select.rows.size(), (size_t)1);
        CHECK_EQ(r2.select.rows[0][0].to_string(), std::string("two"));
    }
}
