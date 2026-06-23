// Abstract logging interface.
//
// The heap file needs to write WAL records before it modifies a page, but the
// storage layer should not depend on the recovery layer. So we define this
// small interface here: the heap file talks to an ILogManager, and the real
// WAL (in recovery/) implements it. This keeps the dependency arrow pointing
// the right way (recovery -> storage, not the reverse).
#pragma once

#include <cstdint>
#include <vector>

#include "minidb/constants.h"
#include "minidb/rid.h"

namespace minidb {

class ILogManager {
public:
    virtual ~ILogManager() = default;

    // Record that transaction `txn` inserted `after_image` at `rid` in file
    // `file_id`. Returns the LSN assigned to the record.
    virtual lsn_t log_insert(txn_id_t txn, int file_id, const RID& rid,
                             const std::vector<uint8_t>& after_image) = 0;

    // Record that transaction `txn` deleted the record at `rid` (whose bytes
    // were `before_image`). Returns the LSN assigned to the record.
    virtual lsn_t log_delete(txn_id_t txn, int file_id, const RID& rid,
                             const std::vector<uint8_t>& before_image) = 0;
};

}  // namespace minidb
