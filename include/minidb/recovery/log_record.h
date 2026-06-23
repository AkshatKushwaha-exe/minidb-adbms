// A single Write-Ahead-Log record.
//
// Every change that must survive a crash is described by one of these records
// and written to the log *before* the corresponding page is allowed to reach
// disk (write-ahead logging). On INSERT we store the new bytes (the "after
// image"); on DELETE we store the old bytes (the "before image") so the change
// can be undone during recovery.
#pragma once

#include <cstdint>
#include <vector>

#include "minidb/constants.h"
#include "minidb/rid.h"

namespace minidb {

enum class LogType : uint8_t {
    BEGIN = 1,
    COMMIT = 2,
    ABORT = 3,
    INSERT = 4,
    DELETE = 5,
    CHECKPOINT = 6,
};

struct LogRecord {
    lsn_t lsn = INVALID_LSN;
    LogType type = LogType::BEGIN;
    txn_id_t txn = INVALID_TXN_ID;

    // Only meaningful for INSERT / DELETE:
    int file_id = -1;       // stable table id (== catalog id)
    RID rid;                // where the record lives
    std::vector<uint8_t> image;  // after-image (INSERT) or before-image (DELETE)
};

}  // namespace minidb
