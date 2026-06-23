#include "minidb/record/schema.h"

#include <cstring>

#include "minidb/exceptions.h"

namespace minidb {

int Schema::column_index(const std::string& name) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

std::vector<uint8_t> Schema::serialize(const Tuple& tuple) const {
    if (tuple.size() != columns_.size()) {
        throw DBException("serialize: tuple has wrong number of columns");
    }
    std::vector<uint8_t> out;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const Column& col = columns_[i];
        const Value& val = tuple[i];
        if (val.type() != col.type) {
            throw DBException("serialize: column '" + col.name +
                              "' type mismatch");
        }
        if (col.type == Type::INT) {
            int64_t v = val.as_int();
            uint8_t buf[8];
            std::memcpy(buf, &v, 8);
            out.insert(out.end(), buf, buf + 8);
        } else {  // TEXT
            const std::string& s = val.as_text();
            uint32_t len = static_cast<uint32_t>(s.size());
            uint8_t buf[4];
            std::memcpy(buf, &len, 4);
            out.insert(out.end(), buf, buf + 4);
            out.insert(out.end(), s.begin(), s.end());
        }
    }
    return out;
}

Tuple Schema::deserialize(const std::vector<uint8_t>& bytes) const {
    Tuple tuple;
    std::size_t pos = 0;
    for (const Column& col : columns_) {
        if (col.type == Type::INT) {
            if (pos + 8 > bytes.size())
                throw DBException("deserialize: truncated INT");
            int64_t v;
            std::memcpy(&v, bytes.data() + pos, 8);
            pos += 8;
            tuple.push_back(Value::make_int(v));
        } else {  // TEXT
            if (pos + 4 > bytes.size())
                throw DBException("deserialize: truncated TEXT length");
            uint32_t len;
            std::memcpy(&len, bytes.data() + pos, 4);
            pos += 4;
            if (pos + len > bytes.size())
                throw DBException("deserialize: truncated TEXT body");
            std::string s(bytes.begin() + pos, bytes.begin() + pos + len);
            pos += len;
            tuple.push_back(Value::make_text(std::move(s)));
        }
    }
    return tuple;
}

}  // namespace minidb
