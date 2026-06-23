// A hand-written lexer + recursive-descent parser for MiniDB's SQL subset.
//
// Recursive descent is used because it maps cleanly onto the grammar and is
// easy to read and explain -- each grammar rule becomes one parse_* method.
#pragma once

#include <string>
#include <vector>

#include "minidb/query/ast.h"

namespace minidb {

enum class TokKind { IDENT, NUMBER, STRING, SYMBOL, KEYWORD, END };

struct Token {
    TokKind kind;
    std::string text;   // identifier name, keyword (upper-cased), symbol, etc.
    long long number = 0;  // value if kind == NUMBER
};

class Parser {
public:
    // Parse a single SQL statement (a trailing ';' is optional).
    static Statement parse(const std::string& sql);

private:
    explicit Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

    Statement parse_statement();
    Statement parse_create();
    Statement parse_insert();
    Statement parse_select();
    Statement parse_delete();

    Predicate parse_predicate();
    std::vector<Predicate> parse_where();
    ColumnRef parse_column_ref();
    Value parse_value();

    // Token cursor helpers.
    const Token& peek() const { return toks_[pos_]; }
    const Token& advance() { return toks_[pos_++]; }
    bool is_keyword(const std::string& kw) const;
    bool is_symbol(const std::string& s) const;
    bool accept_keyword(const std::string& kw);
    bool accept_symbol(const std::string& s);
    void expect_symbol(const std::string& s);
    void expect_keyword(const std::string& kw);
    std::string expect_ident();

    std::vector<Token> toks_;
    std::size_t pos_ = 0;
};

}  // namespace minidb
