// A slotted page -- the unit of storage that lives inside a heap file.
//
// Layout (all integers stored little-endian via memcpy):
//
//   offset  size  field
//   ------  ----  -----------------------------------------------------------
//   0       8     page_lsn        log sequence number of last change (recovery)
//   8       2     num_slots       number of slot-directory entries
//   10      2     free_ptr        offset where the free space *ends* (records
//                                 are packed from the end of the page toward
//                                 the front; free_ptr starts at PAGE_SIZE)
//   12      ...   slot directory  num_slots entries of (offset:2, length:2)
//   ...           free space
//   ...           record data (grows downward from the end of the page)
//
// A slot with length == 0 is a tombstone (the record was deleted). We never
// physically compact within this project; deletes leave a hole. This keeps RIDs
// stable, which matters because the B+ tree stores RIDs.  Compaction is noted
// as future work in the docs.
//
// The slotted design is what real systems (PostgreSQL, SQLite) use to store
// variable-length tuples while still addressing each by a small slot number.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "minidb/constants.h"

namespace minidb {

class Page {
public:
    // Byte offsets of the header fields.
    static constexpr std::size_t OFFSET_LSN = 0;
    static constexpr std::size_t OFFSET_NUM_SLOTS = 8;
    static constexpr std::size_t OFFSET_FREE_PTR = 10;
    static constexpr std::size_t HEADER_SIZE = 12;
    static constexpr std::size_t SLOT_SIZE = 4;  // 2 bytes offset + 2 length

    // A fresh, empty page.
    Page();

    // Wrap an existing PAGE_SIZE byte buffer read from disk.
    explicit Page(const std::vector<uint8_t>& bytes);

    // Raw access for the disk manager / buffer pool.
    uint8_t* data() { return data_.data(); }
    const uint8_t* data() const { return data_.data(); }
    const std::vector<uint8_t>& bytes() const { return data_; }

    // --- header accessors ---------------------------------------------------
    lsn_t lsn() const;
    void set_lsn(lsn_t lsn);
    uint16_t num_slots() const;

    // --- record operations --------------------------------------------------
    // Free contiguous bytes available for a new record + its slot entry.
    std::size_t free_space() const;

    // Append a record; returns the new slot number, or -1 if it does not fit.
    int insert_record(const std::vector<uint8_t>& record);

    // Recovery helper: place a record at an *exact* slot, growing the slot
    // directory if needed. Used during REDO so that RIDs are reproduced
    // deterministically. Returns false if it does not fit.
    bool insert_record_at(int slot, const std::vector<uint8_t>& record);

    // Fetch the record stored in `slot`. Returns false if the slot is out of
    // range or has been deleted (tombstone).
    bool get_record(int slot, std::vector<uint8_t>& out) const;

    // Tombstone the record in `slot`. Returns false if already deleted / bad.
    bool delete_record(int slot);

    // True if the slot exists and currently holds a live record.
    bool is_slot_live(int slot) const;

    // A valid (initialised) page always has free_ptr > 0. An all-zero page
    // read from disk -- allocated but never written before a crash -- has
    // free_ptr == 0; recovery uses this to detect and re-initialise it.
    bool is_initialized() const { return free_ptr() != 0; }

private:
    uint16_t read_u16(std::size_t off) const;
    void write_u16(std::size_t off, uint16_t v);
    uint16_t free_ptr() const;
    void set_free_ptr(uint16_t v);
    void set_num_slots(uint16_t v);
    void slot_entry(int slot, uint16_t& offset, uint16_t& length) const;
    void set_slot_entry(int slot, uint16_t offset, uint16_t length);

    std::vector<uint8_t> data_;  // always exactly PAGE_SIZE bytes
};

}  // namespace minidb
