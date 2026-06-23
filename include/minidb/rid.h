// A Record IDentifier: where a tuple physically lives inside a heap file.
//
// A RID is (page id, slot number). It is stable for the life of the record and
// is exactly what we store in the B+ tree, so an index lookup yields a RID that
// the heap file can resolve directly.
#pragma once

#include <cstdint>
#include <functional>
#include <ostream>

#include "minidb/constants.h"

namespace minidb {

struct RID {
    page_id_t page_id = INVALID_PAGE_ID;
    int32_t slot = -1;

    bool operator==(const RID& o) const {
        return page_id == o.page_id && slot == o.slot;
    }
    bool operator!=(const RID& o) const { return !(*this == o); }
    bool operator<(const RID& o) const {
        if (page_id != o.page_id) return page_id < o.page_id;
        return slot < o.slot;
    }
    bool is_valid() const { return page_id != INVALID_PAGE_ID && slot >= 0; }
};

inline std::ostream& operator<<(std::ostream& os, const RID& rid) {
    return os << "(" << rid.page_id << "," << rid.slot << ")";
}

}  // namespace minidb

// Allow RID to be used as a key in unordered_map (the lock manager needs this).
namespace std {
template <>
struct hash<minidb::RID> {
    size_t operator()(const minidb::RID& rid) const {
        return (static_cast<size_t>(rid.page_id) << 20) ^
               static_cast<size_t>(rid.slot);
    }
};
}  // namespace std
