// A heap file: an unordered collection of records spread across pages, built
// on top of the buffer pool. This is the physical storage for one table.
//
// Records are addressed by RID = (page_id, slot). Inserts append to a page with
// free space (allocating a new page when needed); deletes tombstone the slot.
//
// If an ILogManager is supplied, every insert/delete first writes a WAL record
// and stamps the page's LSN -- this is what makes crash recovery possible.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "minidb/constants.h"
#include "minidb/log_interface.h"
#include "minidb/rid.h"
#include "minidb/storage/buffer_pool.h"

namespace minidb {

class HeapFile {
public:
    // `file_id` is the id returned when the table's DiskManager was registered
    // with the buffer pool. `log` may be null for storage-only (no recovery).
    HeapFile(BufferPool* bpool, int file_id, ILogManager* log = nullptr);

    // Insert a record, returning its RID. Logs an INSERT if logging is enabled.
    RID insert(const std::vector<uint8_t>& record, txn_id_t txn = INVALID_TXN_ID);

    // Insert at a specific RID. Used ONLY by recovery (redo/undo) to reproduce
    // a record deterministically. Does not log.
    void insert_at(const RID& rid, const std::vector<uint8_t>& record,
                   lsn_t page_lsn);

    // Fetch the record at `rid`. Returns false if missing/deleted.
    bool get(const RID& rid, std::vector<uint8_t>& out);

    // Delete the record at `rid`. Logs a DELETE if logging is enabled.
    // Returns false if the record was already gone.
    bool remove(const RID& rid, txn_id_t txn = INVALID_TXN_ID);

    // Recovery helper: tombstone a slot and stamp the page LSN, no logging.
    void remove_at(const RID& rid, lsn_t page_lsn);

    page_id_t page_count() const;
    int file_id() const { return file_id_; }

    // Forward iterator over all *live* records in the file. Used by SeqScan.
    class Iterator {
    public:
        Iterator(HeapFile* hf, bool end);
        bool operator!=(const Iterator& o) const;
        Iterator& operator++();  // advance to next live record
        // Dereference yields the current (RID, record bytes).
        std::pair<RID, std::vector<uint8_t>> operator*() const;

    private:
        void advance_to_live();
        HeapFile* hf_;
        page_id_t page_id_;
        int slot_;
        bool at_end_;
        RID cur_rid_;
        std::vector<uint8_t> cur_rec_;
    };

    Iterator begin() { return Iterator(this, false); }
    Iterator end() { return Iterator(this, true); }

private:
    BufferPool* bpool_;
    int file_id_;
    ILogManager* log_;
};

}  // namespace minidb
