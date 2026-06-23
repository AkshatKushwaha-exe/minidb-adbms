// Project-wide constants and a couple of fundamental type aliases.
//
// Keeping the magic numbers in one header makes the on-disk format easy to
// reason about and easy to tweak while experimenting.
#pragma once

#include <cstddef>
#include <cstdint>

namespace minidb {

// Size of a single page on disk, in bytes. 4 KiB is the classic choice: it
// matches the page size of most operating systems and SSDs.
constexpr std::size_t PAGE_SIZE = 4096;

// Number of frames the buffer pool keeps resident in memory. Kept small so
// that eviction actually happens during demos and tests.
constexpr std::size_t DEFAULT_BUFFER_POOL_SIZE = 16;

// Maximum number of keys a B+ tree node holds before it must split.
constexpr int BTREE_ORDER = 4;

// A page id is just the page's index within its file (0, 1, 2, ...).
using page_id_t = int32_t;

// A log sequence number uniquely orders WAL records.
using lsn_t = int64_t;

// A transaction id.
using txn_id_t = int64_t;

constexpr page_id_t INVALID_PAGE_ID = -1;
constexpr lsn_t INVALID_LSN = -1;
constexpr txn_id_t INVALID_TXN_ID = -1;

}  // namespace minidb
