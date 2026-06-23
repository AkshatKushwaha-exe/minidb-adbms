#include "minidb/catalog/catalog.h"

#include <fstream>
#include <sstream>

#include "minidb/exceptions.h"

namespace minidb {

void Catalog::create_table(const std::string& name, const Schema& schema) {
    if (tables_.count(name)) {
        throw CatalogException("table '" + name + "' already exists");
    }
    TableInfo info;
    info.name = name;
    info.schema = schema;
    info.id = next_id_++;
    // Every table gets an automatic primary-key index on its PK column.
    IndexInfo pk;
    pk.name = name + "_pk";
    pk.column = schema.primary_key_index();
    pk.unique = true;
    pk.primary = true;
    info.indexes.push_back(pk);
    tables_[name] = std::move(info);
}

void Catalog::add_index(const std::string& table, const IndexInfo& index) {
    auto it = tables_.find(table);
    if (it == tables_.end()) {
        throw CatalogException("add_index: no such table '" + table + "'");
    }
    for (const auto& existing : it->second.indexes) {
        if (existing.name == index.name) {
            throw CatalogException("index '" + index.name + "' already exists");
        }
    }
    it->second.indexes.push_back(index);
}

bool Catalog::has_table(const std::string& name) const {
    return tables_.count(name) > 0;
}

const TableInfo& Catalog::get_table(const std::string& name) const {
    auto it = tables_.find(name);
    if (it == tables_.end()) {
        throw CatalogException("no such table '" + name + "'");
    }
    return it->second;
}

std::vector<std::string> Catalog::table_names() const {
    std::vector<std::string> names;
    for (const auto& kv : tables_) names.push_back(kv.first);
    return names;
}

// --- persistence ------------------------------------------------------------
// Simple, human-readable line format (easy to inspect during the viva):
//
//   TABLE <name> <num_cols> <pk_index> <num_indexes>
//   COL <name> <INT|TEXT>
//   ... (num_cols lines)
//   INDEX <name> <column> <unique 0/1> <primary 0/1>
//   ... (num_indexes lines)

void Catalog::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) throw CatalogException("cannot write catalog to " + path);
    out << "META " << next_id_ << "\n";
    for (const auto& kv : tables_) {
        const TableInfo& t = kv.second;
        out << "TABLE " << t.name << " " << t.id << " "
            << t.schema.num_columns() << " " << t.schema.primary_key_index()
            << " " << t.indexes.size() << "\n";
        for (const auto& col : t.schema.columns()) {
            out << "COL " << col.name << " " << type_name(col.type) << "\n";
        }
        for (const auto& idx : t.indexes) {
            out << "INDEX " << idx.name << " " << idx.column << " "
                << (idx.unique ? 1 : 0) << " " << (idx.primary ? 1 : 0) << "\n";
        }
    }
}

void Catalog::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;  // no catalog yet: start empty (fresh database)
    tables_.clear();

    std::string keyword;
    while (in >> keyword) {
        if (keyword == "META") {
            in >> next_id_;
            continue;
        }
        if (keyword != "TABLE") {
            throw CatalogException("corrupt catalog: expected TABLE");
        }
        std::string name;
        int id, num_cols, pk_index, num_indexes;
        in >> name >> id >> num_cols >> pk_index >> num_indexes;

        std::vector<Column> cols;
        for (int i = 0; i < num_cols; ++i) {
            std::string kw, cname, ctype;
            in >> kw >> cname >> ctype;
            Type t = (ctype == "INT") ? Type::INT : Type::TEXT;
            cols.push_back({cname, t});
        }
        TableInfo info;
        info.name = name;
        info.id = id;
        info.schema = Schema(cols, pk_index);
        for (int i = 0; i < num_indexes; ++i) {
            std::string kw, iname;
            int column, unique, primary;
            in >> kw >> iname >> column >> unique >> primary;
            info.indexes.push_back(
                {iname, column, unique != 0, primary != 0});
        }
        tables_[name] = std::move(info);
    }
}

}  // namespace minidb
