// MiniDB interactive shell.
//
// Reads SQL statements (terminated by ';') from standard input, runs them and
// prints the results. Also accepts a few dot-commands:
//   .help            show help
//   .tables          list tables
//   .stats           buffer-pool hit/miss statistics
//   .exit / .quit    leave
//
// Usage:  ./minidb [data_dir]     (data_dir defaults to "data")
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "minidb/engine.h"
#include "minidb/exceptions.h"

using namespace minidb;

static void print_select(const SelectResult& r) {
    // Compute column widths.
    std::vector<std::size_t> w(r.columns.size());
    for (std::size_t i = 0; i < r.columns.size(); ++i) w[i] = r.columns[i].size();
    for (const auto& row : r.rows)
        for (std::size_t i = 0; i < row.size(); ++i)
            w[i] = std::max(w[i], row[i].to_string().size());

    auto bar = [&]() {
        std::cout << "+";
        for (std::size_t i = 0; i < w.size(); ++i) {
            std::cout << std::string(w[i] + 2, '-') << "+";
        }
        std::cout << "\n";
    };
    auto pad = [](const std::string& s, std::size_t width) {
        return s + std::string(width - s.size(), ' ');
    };

    bar();
    std::cout << "|";
    for (std::size_t i = 0; i < r.columns.size(); ++i)
        std::cout << " " << pad(r.columns[i], w[i]) << " |";
    std::cout << "\n";
    bar();
    for (const auto& row : r.rows) {
        std::cout << "|";
        for (std::size_t i = 0; i < row.size(); ++i)
            std::cout << " " << pad(row[i].to_string(), w[i]) << " |";
        std::cout << "\n";
    }
    bar();
    std::cout << r.rows.size() << " row(s)\n";
}

static void run_statement(Engine& db, const std::string& sql) {
    try {
        QueryResult res = db.execute(sql);
        switch (res.kind) {
            case QueryResult::Kind::SELECT:
                print_select(res.select);
                break;
            case QueryResult::Kind::MODIFY:
                std::cout << res.affected << " row(s) affected\n";
                break;
            case QueryResult::Kind::EXPLAIN:
                std::cout << "Query plan:\n" << res.message;
                break;
            case QueryResult::Kind::MESSAGE:
                std::cout << res.message << "\n";
                break;
        }
    } catch (const DBException& e) {
        std::cout << "Error: " << e.what() << "\n";
    }
}

int main(int argc, char** argv) {
    std::string data_dir = (argc > 1) ? argv[1] : "data";
    Engine db(data_dir);

    std::cout << "MiniDB shell. Type SQL ending in ';', or .help. .exit to quit.\n";

    std::string buffer;
    std::string line;
    bool interactive = true;
    while (true) {
        if (interactive) std::cout << (db.in_transaction() ? "minidb*> " : "minidb> ");
        if (!std::getline(std::cin, line)) break;

        // Dot-commands (only when not mid-statement). A buffer holding only
        // leftover whitespace counts as "not mid-statement".
        bool buffer_blank =
            buffer.find_first_not_of(" \t\n\r") == std::string::npos;
        if (buffer_blank) buffer.clear();
        std::string trimmed = line;
        std::size_t a = trimmed.find_first_not_of(" \t");
        if (buffer_blank && a != std::string::npos && trimmed[a] == '.') {
            std::string cmd = trimmed.substr(a);
            if (cmd == ".exit" || cmd == ".quit") break;
            if (cmd == ".help") {
                std::cout << "Commands: .tables .stats .exit\n"
                             "SQL: CREATE TABLE/INDEX, INSERT, SELECT (WHERE/JOIN), "
                             "DELETE, BEGIN/COMMIT/ABORT, EXPLAIN <select>\n";
                continue;
            }
            if (cmd == ".tables") {
                auto names = db.table_names();
                if (names.empty()) std::cout << "(no tables)\n";
                for (const auto& n : names) std::cout << n << "\n";
                continue;
            }
            if (cmd == ".stats") {
                std::cout << "buffer pool: " << db.buffer_pool().hits()
                          << " hits, " << db.buffer_pool().misses()
                          << " misses, capacity " << db.buffer_pool().capacity()
                          << "\n";
                continue;
            }
            std::cout << "unknown command: " << cmd << "\n";
            continue;
        }

        buffer += line + "\n";
        // Execute every complete (;-terminated) statement in the buffer.
        std::size_t semi;
        while ((semi = buffer.find(';')) != std::string::npos) {
            std::string stmt = buffer.substr(0, semi);
            buffer.erase(0, semi + 1);
            std::size_t s = stmt.find_first_not_of(" \t\n\r");
            if (s != std::string::npos) run_statement(db, stmt);
        }
    }
    return 0;
}
