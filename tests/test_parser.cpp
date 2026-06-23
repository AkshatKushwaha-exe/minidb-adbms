// Tests for the SQL lexer + parser.
#include "minidb/exceptions.h"
#include "minidb/query/parser.h"
#include "test_framework.h"

using namespace minidb;

TEST(parser, create_table) {
    Statement s = Parser::parse(
        "CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT)");
    CHECK(s.type == StmtType::CREATE_TABLE);
    CHECK_EQ(s.create_table.table, std::string("users"));
    CHECK_EQ(s.create_table.columns.size(), (size_t)3);
    CHECK_EQ(s.create_table.columns[0].name, std::string("id"));
    CHECK(s.create_table.columns[0].type == Type::INT);
    CHECK(s.create_table.columns[0].primary_key);
    CHECK(s.create_table.columns[1].type == Type::TEXT);
    CHECK(!s.create_table.columns[1].primary_key);
}

TEST(parser, create_index) {
    Statement s = Parser::parse("CREATE INDEX idx_name ON users (name)");
    CHECK(s.type == StmtType::CREATE_INDEX);
    CHECK_EQ(s.create_index.index_name, std::string("idx_name"));
    CHECK_EQ(s.create_index.table, std::string("users"));
    CHECK_EQ(s.create_index.column, std::string("name"));
}

TEST(parser, insert_single_and_multi) {
    Statement s = Parser::parse(
        "INSERT INTO users (id, name) VALUES (1, 'alice'), (2, 'bob')");
    CHECK(s.type == StmtType::INSERT);
    CHECK_EQ(s.insert.columns.size(), (size_t)2);
    CHECK_EQ(s.insert.rows.size(), (size_t)2);
    CHECK_EQ(s.insert.rows[0][0].as_int(), (int64_t)1);
    CHECK_EQ(s.insert.rows[0][1].as_text(), std::string("alice"));
    CHECK_EQ(s.insert.rows[1][1].as_text(), std::string("bob"));
}

TEST(parser, insert_negative_number) {
    Statement s = Parser::parse("INSERT INTO t VALUES (-42, 'x')");
    CHECK_EQ(s.insert.rows[0][0].as_int(), (int64_t)-42);
}

TEST(parser, select_star_where) {
    Statement s = Parser::parse("SELECT * FROM users WHERE age >= 18");
    CHECK(s.type == StmtType::SELECT);
    CHECK(s.select.select_star);
    CHECK_EQ(s.select.from_table, std::string("users"));
    CHECK_EQ(s.select.where.size(), (size_t)1);
    CHECK(s.select.where[0].op == CompOp::GE);
    CHECK_EQ(s.select.where[0].left.col, std::string("age"));
    CHECK(!s.select.where[0].right_is_column);
    CHECK_EQ(s.select.where[0].right_value.as_int(), (int64_t)18);
}

TEST(parser, select_columns_and_and) {
    Statement s = Parser::parse(
        "SELECT id, name FROM users WHERE age > 20 AND name = 'bob'");
    CHECK(!s.select.select_star);
    CHECK_EQ(s.select.columns.size(), (size_t)2);
    CHECK_EQ(s.select.where.size(), (size_t)2);
    CHECK(s.select.where[1].op == CompOp::EQ);
    CHECK_EQ(s.select.where[1].right_value.as_text(), std::string("bob"));
}

TEST(parser, select_join) {
    Statement s = Parser::parse(
        "SELECT u.name, o.item FROM users u JOIN orders o ON u.id = o.uid "
        "WHERE o.item = 'book'");
    CHECK_EQ(s.select.from_table, std::string("users"));
    CHECK_EQ(s.select.from_alias, std::string("u"));
    CHECK_EQ(s.select.joins.size(), (size_t)1);
    CHECK_EQ(s.select.joins[0].table, std::string("orders"));
    CHECK_EQ(s.select.joins[0].alias, std::string("o"));
    CHECK(s.select.joins[0].on.right_is_column);
    CHECK_EQ(s.select.joins[0].on.left.table, std::string("u"));
    CHECK_EQ(s.select.joins[0].on.right_col.table, std::string("o"));
    CHECK_EQ(s.select.columns[0].table, std::string("u"));
}

TEST(parser, delete_with_where) {
    Statement s = Parser::parse("DELETE FROM users WHERE id = 5");
    CHECK(s.type == StmtType::DELETE);
    CHECK_EQ(s.del.table, std::string("users"));
    CHECK_EQ(s.del.where.size(), (size_t)1);
    CHECK_EQ(s.del.where[0].right_value.as_int(), (int64_t)5);
}

TEST(parser, txn_control) {
    CHECK(Parser::parse("BEGIN").type == StmtType::BEGIN);
    CHECK(Parser::parse("BEGIN TRANSACTION;").type == StmtType::BEGIN);
    CHECK(Parser::parse("COMMIT;").type == StmtType::COMMIT);
    CHECK(Parser::parse("ABORT").type == StmtType::ABORT);
    CHECK(Parser::parse("ROLLBACK").type == StmtType::ABORT);
}

TEST(parser, case_insensitive_keywords) {
    Statement s = Parser::parse("select * from users where id = 1");
    CHECK(s.type == StmtType::SELECT);
    CHECK(s.select.select_star);
}

TEST(parser, errors) {
    CHECK_THROWS(Parser::parse("SELECT FROM users"));      // no select list
    CHECK_THROWS(Parser::parse("CREATE TABLE t (id FOO)"));  // bad type
    CHECK_THROWS(Parser::parse("INSERT INTO t VALUES"));     // no values
    CHECK_THROWS(Parser::parse("SELECT * FROM"));            // no table
    CHECK_THROWS(Parser::parse("garbage"));                  // unknown stmt
}
