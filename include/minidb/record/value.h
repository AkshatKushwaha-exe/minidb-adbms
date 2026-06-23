// A Value is a single typed cell in a row. MiniDB supports two column types --
// 64-bit integers and variable-length text -- which is enough to demonstrate
// every database concept in the project while keeping (de)serialisation simple.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>

#include "minidb/exceptions.h"

namespace minidb {

enum class Type { INT, TEXT };

inline std::string type_name(Type t) {
    return t == Type::INT ? "INT" : "TEXT";
}

class Value {
public:
    Value() : type_(Type::INT), int_(0) {}  // default: INT 0

    static Value make_int(int64_t v) {
        Value val;
        val.type_ = Type::INT;
        val.int_ = v;
        return val;
    }
    static Value make_text(std::string s) {
        Value val;
        val.type_ = Type::TEXT;
        val.text_ = std::move(s);
        return val;
    }

    Type type() const { return type_; }

    int64_t as_int() const {
        if (type_ != Type::INT) throw DBException("Value is not an INT");
        return int_;
    }
    const std::string& as_text() const {
        if (type_ != Type::TEXT) throw DBException("Value is not TEXT");
        return text_;
    }

    // Ordering / equality. Only values of the same type may be compared; the
    // binder guarantees this before comparisons reach here.
    bool operator==(const Value& o) const {
        if (type_ != o.type_) return false;
        return type_ == Type::INT ? int_ == o.int_ : text_ == o.text_;
    }
    bool operator!=(const Value& o) const { return !(*this == o); }
    bool operator<(const Value& o) const {
        if (type_ != o.type_)
            throw DBException("cannot compare values of different types");
        return type_ == Type::INT ? int_ < o.int_ : text_ < o.text_;
    }
    bool operator<=(const Value& o) const { return *this < o || *this == o; }
    bool operator>(const Value& o) const { return !(*this <= o); }
    bool operator>=(const Value& o) const { return !(*this < o); }

    std::string to_string() const {
        return type_ == Type::INT ? std::to_string(int_) : text_;
    }

private:
    Type type_;
    int64_t int_ = 0;
    std::string text_;
};

inline std::ostream& operator<<(std::ostream& os, const Value& v) {
    return os << v.to_string();
}

}  // namespace minidb
