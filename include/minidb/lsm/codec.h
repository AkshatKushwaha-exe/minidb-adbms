// Encoding helpers for the LSM engine.
//
// Keys are `Value`s (INT or TEXT). We need a self-describing byte encoding so a
// key can be written to / read back from an SSTable or the LSM write-ahead log.
// Layout: [type:1][payload], where INT payload is 8 bytes and TEXT payload is a
// 4-byte length followed by the UTF-8 bytes.
#pragma once

#include <cstdint>
#include <cstring>
#include <istream>
#include <ostream>
#include <vector>

#include "minidb/record/value.h"

namespace minidb {
namespace lsm {

// Append the encoding of `v` to `out`.
inline void encode_value(const Value& v, std::vector<uint8_t>& out) {
    if (v.type() == Type::INT) {
        out.push_back(0);
        int64_t x = v.as_int();
        uint8_t b[8];
        std::memcpy(b, &x, 8);
        out.insert(out.end(), b, b + 8);
    } else {
        out.push_back(1);
        const std::string& s = v.as_text();
        uint32_t len = static_cast<uint32_t>(s.size());
        uint8_t b[4];
        std::memcpy(b, &len, 4);
        out.insert(out.end(), b, b + 4);
        out.insert(out.end(), s.begin(), s.end());
    }
}

// Decode a Value starting at `pos` in `buf`; advances `pos` past it.
inline Value decode_value(const std::vector<uint8_t>& buf, std::size_t& pos) {
    uint8_t type = buf.at(pos++);
    if (type == 0) {
        int64_t x;
        std::memcpy(&x, buf.data() + pos, 8);
        pos += 8;
        return Value::make_int(x);
    }
    uint32_t len;
    std::memcpy(&len, buf.data() + pos, 4);
    pos += 4;
    std::string s(buf.begin() + pos, buf.begin() + pos + len);
    pos += len;
    return Value::make_text(std::move(s));
}

// Stream helpers (used by SSTable I/O).
inline void write_value(std::ostream& os, const Value& v) {
    std::vector<uint8_t> b;
    encode_value(v, b);
    os.write(reinterpret_cast<const char*>(b.data()),
             static_cast<std::streamsize>(b.size()));
}

// Read one Value from a stream; returns false on EOF/short read.
inline bool read_value(std::istream& is, Value& out) {
    int type = is.get();
    if (type == EOF) return false;
    if (type == 0) {
        int64_t x;
        if (!is.read(reinterpret_cast<char*>(&x), 8)) return false;
        out = Value::make_int(x);
        return true;
    }
    uint32_t len;
    if (!is.read(reinterpret_cast<char*>(&len), 4)) return false;
    std::string s(len, '\0');
    if (len > 0 && !is.read(&s[0], len)) return false;
    out = Value::make_text(std::move(s));
    return true;
}

}  // namespace lsm
}  // namespace minidb
