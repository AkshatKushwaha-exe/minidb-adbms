// A Schema describes the columns of a table and which column is the primary
// key. A Tuple is just a row -- a vector of Values in column order.
//
// The Schema also knows how to serialise a Tuple to the byte string stored in a
// heap-file slot, and back. Layout per column:
//   INT  -> 8 bytes, little-endian
//   TEXT -> 4 byte length, then that many UTF-8 bytes
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "minidb/record/value.h"

namespace minidb {

using Tuple = std::vector<Value>;

struct Column {
    std::string name;
    Type type;
};

class Schema {
public:
    Schema() = default;
    Schema(std::vector<Column> columns, int primary_key_index)
        : columns_(std::move(columns)), pk_index_(primary_key_index) {}

    const std::vector<Column>& columns() const { return columns_; }
    std::size_t num_columns() const { return columns_.size(); }
    int primary_key_index() const { return pk_index_; }
    const Column& column(int i) const { return columns_.at(i); }

    // Returns the index of `name`, or -1 if there is no such column.
    int column_index(const std::string& name) const;

    // Serialise a tuple to bytes (validating it matches the schema).
    std::vector<uint8_t> serialize(const Tuple& tuple) const;

    // Deserialise bytes back into a tuple.
    Tuple deserialize(const std::vector<uint8_t>& bytes) const;

private:
    std::vector<Column> columns_;
    int pk_index_ = 0;
};

}  // namespace minidb
