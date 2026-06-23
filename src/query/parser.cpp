#include "minidb/query/parser.h"

#include <cctype>
#include <set>

#include "minidb/exceptions.h"

namespace minidb {

namespace {

const std::set<std::string>& keywords() {
    static const std::set<std::string> kw = {
        "CREATE", "TABLE",  "INDEX",  "ON",     "INSERT", "INTO",
        "VALUES", "SELECT", "FROM",   "WHERE",  "JOIN",   "INNER",
        "DELETE", "BEGIN",  "COMMIT", "ABORT",  "ROLLBACK", "AND",
        "PRIMARY","KEY",    "INT",    "INTEGER","TEXT",   "AS",
        "TRANSACTION"};
    return kw;
}

std::string to_upper(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = static_cast<char>(std::toupper((unsigned char)c));
    return r;
}

std::vector<Token> tokenize(const std::string& sql) {
    std::vector<Token> toks;
    std::size_t i = 0, n = sql.size();
    while (i < n) {
        char c = sql[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }

        if (std::isalpha((unsigned char)c) || c == '_') {
            std::size_t j = i;
            while (j < n && (std::isalnum((unsigned char)sql[j]) || sql[j] == '_'))
                ++j;
            std::string word = sql.substr(i, j - i);
            std::string up = to_upper(word);
            if (keywords().count(up)) {
                toks.push_back({TokKind::KEYWORD, up, 0});
            } else {
                toks.push_back({TokKind::IDENT, word, 0});
            }
            i = j;
            continue;
        }
        if (std::isdigit((unsigned char)c)) {
            std::size_t j = i;
            while (j < n && std::isdigit((unsigned char)sql[j])) ++j;
            Token t{TokKind::NUMBER, sql.substr(i, j - i), 0};
            t.number = std::stoll(t.text);
            toks.push_back(t);
            i = j;
            continue;
        }
        if (c == '\'') {
            std::size_t j = i + 1;
            std::string val;
            while (j < n && sql[j] != '\'') val.push_back(sql[j++]);
            if (j >= n) throw SQLException("unterminated string literal");
            toks.push_back({TokKind::STRING, val, 0});
            i = j + 1;
            continue;
        }
        // Multi-character operators first.
        auto two = [&](const char* op) {
            return i + 1 < n && sql[i] == op[0] && sql[i + 1] == op[1];
        };
        if (two("!=")) { toks.push_back({TokKind::SYMBOL, "!=", 0}); i += 2; continue; }
        if (two("<>")) { toks.push_back({TokKind::SYMBOL, "!=", 0}); i += 2; continue; }
        if (two("<=")) { toks.push_back({TokKind::SYMBOL, "<=", 0}); i += 2; continue; }
        if (two(">=")) { toks.push_back({TokKind::SYMBOL, ">=", 0}); i += 2; continue; }

        std::string sym(1, c);
        if (std::string("()*,.;=<>-").find(c) != std::string::npos) {
            toks.push_back({TokKind::SYMBOL, sym, 0});
            ++i;
            continue;
        }
        throw SQLException(std::string("unexpected character '") + c + "'");
    }
    toks.push_back({TokKind::END, "", 0});
    return toks;
}

CompOp symbol_to_op(const std::string& s) {
    if (s == "=") return CompOp::EQ;
    if (s == "!=") return CompOp::NE;
    if (s == "<") return CompOp::LT;
    if (s == "<=") return CompOp::LE;
    if (s == ">") return CompOp::GT;
    if (s == ">=") return CompOp::GE;
    throw SQLException("expected a comparison operator, got '" + s + "'");
}

}  // namespace

// --- token helpers ----------------------------------------------------------
bool Parser::is_keyword(const std::string& kw) const {
    return peek().kind == TokKind::KEYWORD && peek().text == kw;
}
bool Parser::is_symbol(const std::string& s) const {
    return peek().kind == TokKind::SYMBOL && peek().text == s;
}
bool Parser::accept_keyword(const std::string& kw) {
    if (is_keyword(kw)) { advance(); return true; }
    return false;
}
bool Parser::accept_symbol(const std::string& s) {
    if (is_symbol(s)) { advance(); return true; }
    return false;
}
void Parser::expect_symbol(const std::string& s) {
    if (!accept_symbol(s)) throw SQLException("expected '" + s + "'");
}
void Parser::expect_keyword(const std::string& kw) {
    if (!accept_keyword(kw)) throw SQLException("expected keyword " + kw);
}
std::string Parser::expect_ident() {
    if (peek().kind != TokKind::IDENT)
        throw SQLException("expected an identifier, got '" + peek().text + "'");
    return advance().text;
}

// --- entry point ------------------------------------------------------------
Statement Parser::parse(const std::string& sql) {
    Parser p(tokenize(sql));
    Statement s = p.parse_statement();
    p.accept_symbol(";");
    if (p.peek().kind != TokKind::END)
        throw SQLException("unexpected tokens after statement");
    return s;
}

Statement Parser::parse_statement() {
    if (is_keyword("CREATE")) return parse_create();
    if (is_keyword("INSERT")) return parse_insert();
    if (is_keyword("SELECT")) return parse_select();
    if (is_keyword("DELETE")) return parse_delete();
    if (accept_keyword("BEGIN")) {
        accept_keyword("TRANSACTION");
        return Statement{StmtType::BEGIN, {}, {}, {}, {}, {}};
    }
    if (accept_keyword("COMMIT"))
        return Statement{StmtType::COMMIT, {}, {}, {}, {}, {}};
    if (accept_keyword("ABORT") || accept_keyword("ROLLBACK"))
        return Statement{StmtType::ABORT, {}, {}, {}, {}, {}};
    throw SQLException("unrecognised statement starting with '" +
                       peek().text + "'");
}

Statement Parser::parse_create() {
    expect_keyword("CREATE");
    Statement s;
    if (accept_keyword("TABLE")) {
        s.type = StmtType::CREATE_TABLE;
        s.create_table.table = expect_ident();
        expect_symbol("(");
        do {
            ColumnDef col;
            col.name = expect_ident();
            if (accept_keyword("INT") || accept_keyword("INTEGER"))
                col.type = Type::INT;
            else if (accept_keyword("TEXT"))
                col.type = Type::TEXT;
            else
                throw SQLException("expected a column type (INT or TEXT)");
            if (accept_keyword("PRIMARY")) {
                expect_keyword("KEY");
                col.primary_key = true;
            }
            s.create_table.columns.push_back(col);
        } while (accept_symbol(","));
        expect_symbol(")");
        return s;
    }
    if (accept_keyword("INDEX")) {
        s.type = StmtType::CREATE_INDEX;
        s.create_index.index_name = expect_ident();
        expect_keyword("ON");
        s.create_index.table = expect_ident();
        expect_symbol("(");
        s.create_index.column = expect_ident();
        expect_symbol(")");
        return s;
    }
    throw SQLException("expected TABLE or INDEX after CREATE");
}

Statement Parser::parse_insert() {
    expect_keyword("INSERT");
    expect_keyword("INTO");
    Statement s;
    s.type = StmtType::INSERT;
    s.insert.table = expect_ident();
    if (accept_symbol("(")) {
        do {
            s.insert.columns.push_back(expect_ident());
        } while (accept_symbol(","));
        expect_symbol(")");
    }
    expect_keyword("VALUES");
    do {
        expect_symbol("(");
        std::vector<Value> row;
        do {
            row.push_back(parse_value());
        } while (accept_symbol(","));
        expect_symbol(")");
        s.insert.rows.push_back(std::move(row));
    } while (accept_symbol(","));
    return s;
}

Statement Parser::parse_select() {
    expect_keyword("SELECT");
    Statement s;
    s.type = StmtType::SELECT;
    if (accept_symbol("*")) {
        s.select.select_star = true;
    } else {
        do {
            s.select.columns.push_back(parse_column_ref());
        } while (accept_symbol(","));
    }
    expect_keyword("FROM");
    s.select.from_table = expect_ident();
    // Optional table alias: "FROM t a" or "FROM t AS a".
    if (accept_keyword("AS")) {
        s.select.from_alias = expect_ident();
    } else if (peek().kind == TokKind::IDENT) {
        s.select.from_alias = advance().text;
    }
    // Zero or more joins.
    while (is_keyword("JOIN") || is_keyword("INNER")) {
        accept_keyword("INNER");
        expect_keyword("JOIN");
        JoinClause j;
        j.table = expect_ident();
        if (accept_keyword("AS")) {
            j.alias = expect_ident();
        } else if (peek().kind == TokKind::IDENT) {
            j.alias = advance().text;
        }
        expect_keyword("ON");
        j.on = parse_predicate();
        s.select.joins.push_back(j);
    }
    if (is_keyword("WHERE")) s.select.where = parse_where();
    return s;
}

Statement Parser::parse_delete() {
    expect_keyword("DELETE");
    expect_keyword("FROM");
    Statement s;
    s.type = StmtType::DELETE;
    s.del.table = expect_ident();
    if (is_keyword("WHERE")) s.del.where = parse_where();
    return s;
}

std::vector<Predicate> Parser::parse_where() {
    expect_keyword("WHERE");
    std::vector<Predicate> preds;
    do {
        preds.push_back(parse_predicate());
    } while (accept_keyword("AND"));
    return preds;
}

Predicate Parser::parse_predicate() {
    Predicate p;
    p.left = parse_column_ref();
    if (peek().kind != TokKind::SYMBOL)
        throw SQLException("expected comparison operator in condition");
    p.op = symbol_to_op(advance().text);
    // Right side: a literal value, or another column reference (joins).
    if (peek().kind == TokKind::NUMBER || peek().kind == TokKind::STRING ||
        is_symbol("-")) {
        p.right_is_column = false;
        p.right_value = parse_value();
    } else {
        p.right_is_column = true;
        p.right_col = parse_column_ref();
    }
    return p;
}

ColumnRef Parser::parse_column_ref() {
    ColumnRef ref;
    std::string first = expect_ident();
    if (accept_symbol(".")) {
        ref.table = first;
        ref.col = expect_ident();
    } else {
        ref.col = first;
    }
    return ref;
}

Value Parser::parse_value() {
    bool negative = accept_symbol("-");
    if (peek().kind == TokKind::NUMBER) {
        long long v = advance().number;
        return Value::make_int(negative ? -v : v);
    }
    if (peek().kind == TokKind::STRING) {
        if (negative) throw SQLException("cannot negate a string literal");
        return Value::make_text(advance().text);
    }
    throw SQLException("expected a literal value");
}

}  // namespace minidb
