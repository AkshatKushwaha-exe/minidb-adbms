// The Write-Ahead Log.
//
// All durable changes are appended here first. The log is the source of truth
// for crash recovery. We implement the WAL "golden rule": a dirty page may not
// be written to disk until every log record up to that page's LSN is durable.
// The buffer pool enforces this by calling flush_to_lsn() (wired up as its log
// flush callback) before writing any page.
//
// On-disk format: a sequence of records, each laid out as
//   [u32 body_len][body], where body =
//   [i64 lsn][u8 type][i64 txn][i32 file_id][i32 page_id][i32 slot]
//   [u32 image_len][image bytes]
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "minidb/log_interface.h"
#include "minidb/recovery/log_record.h"

namespace minidb {

class WAL : public ILogManager {
public:
    explicit WAL(const std::string& path);
    ~WAL();

    // Transaction lifecycle records.
    lsn_t log_begin(txn_id_t txn);
    lsn_t log_commit(txn_id_t txn);  // forces the log to disk (durability)
    lsn_t log_abort(txn_id_t txn);
    lsn_t log_checkpoint();

    // ILogManager: data-change records (called by the heap file).
    lsn_t log_insert(txn_id_t txn, int file_id, const RID& rid,
                     const std::vector<uint8_t>& after_image) override;
    lsn_t log_delete(txn_id_t txn, int file_id, const RID& rid,
                     const std::vector<uint8_t>& before_image) override;

    // Ensure all records with lsn <= target are durably on disk.
    void flush_to_lsn(lsn_t target);
    // Ensure everything appended so far is durable.
    void flush();

    lsn_t last_lsn() const { return next_lsn_ - 1; }
    lsn_t flushed_lsn() const { return flushed_lsn_; }

    // Read every record from the log file (used by recovery at startup).
    static std::vector<LogRecord> read_all(const std::string& path);

private:
    lsn_t append(const LogRecord& rec);

    std::string path_;
    std::ofstream out_;
    lsn_t next_lsn_ = 0;
    lsn_t flushed_lsn_ = INVALID_LSN;
};

}  // namespace minidb
