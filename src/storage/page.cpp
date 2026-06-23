#include "minidb/storage/page.h"

#include "minidb/exceptions.h"

namespace minidb {

Page::Page() : data_(PAGE_SIZE, 0) {
    set_lsn(INVALID_LSN);
    set_num_slots(0);
    set_free_ptr(static_cast<uint16_t>(PAGE_SIZE));
}

Page::Page(const std::vector<uint8_t>& bytes) : data_(bytes) {
    if (data_.size() != PAGE_SIZE) {
        throw StorageException("Page constructed from buffer of wrong size");
    }
}

// --- little-endian helpers --------------------------------------------------
uint16_t Page::read_u16(std::size_t off) const {
    uint16_t v;
    std::memcpy(&v, data_.data() + off, sizeof(v));
    return v;
}

void Page::write_u16(std::size_t off, uint16_t v) {
    std::memcpy(data_.data() + off, &v, sizeof(v));
}

lsn_t Page::lsn() const {
    lsn_t v;
    std::memcpy(&v, data_.data() + OFFSET_LSN, sizeof(v));
    return v;
}

void Page::set_lsn(lsn_t lsn) {
    std::memcpy(data_.data() + OFFSET_LSN, &lsn, sizeof(lsn));
}

uint16_t Page::num_slots() const { return read_u16(OFFSET_NUM_SLOTS); }
void Page::set_num_slots(uint16_t v) { write_u16(OFFSET_NUM_SLOTS, v); }
uint16_t Page::free_ptr() const { return read_u16(OFFSET_FREE_PTR); }
void Page::set_free_ptr(uint16_t v) { write_u16(OFFSET_FREE_PTR, v); }

void Page::slot_entry(int slot, uint16_t& offset, uint16_t& length) const {
    std::size_t base = HEADER_SIZE + static_cast<std::size_t>(slot) * SLOT_SIZE;
    offset = read_u16(base);
    length = read_u16(base + 2);
}

void Page::set_slot_entry(int slot, uint16_t offset, uint16_t length) {
    std::size_t base = HEADER_SIZE + static_cast<std::size_t>(slot) * SLOT_SIZE;
    write_u16(base, offset);
    write_u16(base + 2, length);
}

std::size_t Page::free_space() const {
    std::size_t slot_dir_end =
        HEADER_SIZE + static_cast<std::size_t>(num_slots()) * SLOT_SIZE;
    return static_cast<std::size_t>(free_ptr()) - slot_dir_end;
}

int Page::insert_record(const std::vector<uint8_t>& record) {
    // We need room for the record bytes *and* one new slot-directory entry.
    if (record.size() + SLOT_SIZE > free_space()) {
        return -1;  // does not fit
    }
    uint16_t n = num_slots();
    uint16_t new_free = static_cast<uint16_t>(free_ptr() - record.size());
    std::memcpy(data_.data() + new_free, record.data(), record.size());
    set_free_ptr(new_free);
    set_slot_entry(n, new_free, static_cast<uint16_t>(record.size()));
    set_num_slots(n + 1);
    return n;
}

bool Page::insert_record_at(int slot, const std::vector<uint8_t>& record) {
    uint16_t n = num_slots();
    // Growing the slot directory may consume extra slot entries (for any gap).
    int new_slots_needed = (slot >= n) ? (slot + 1 - n) : 0;
    std::size_t extra_dir = static_cast<std::size_t>(new_slots_needed) * SLOT_SIZE;
    if (record.size() + extra_dir > free_space()) {
        return false;
    }
    // Fill any gap with tombstones (offset 0, length 0).
    for (int s = n; s < slot; ++s) {
        set_slot_entry(s, 0, 0);
    }
    if (slot >= n) {
        set_num_slots(static_cast<uint16_t>(slot + 1));
    }
    uint16_t new_free = static_cast<uint16_t>(free_ptr() - record.size());
    std::memcpy(data_.data() + new_free, record.data(), record.size());
    set_free_ptr(new_free);
    set_slot_entry(slot, new_free, static_cast<uint16_t>(record.size()));
    return true;
}

bool Page::get_record(int slot, std::vector<uint8_t>& out) const {
    if (slot < 0 || slot >= num_slots()) return false;
    uint16_t off, len;
    slot_entry(slot, off, len);
    if (len == 0) return false;  // tombstone
    out.assign(data_.begin() + off, data_.begin() + off + len);
    return true;
}

bool Page::delete_record(int slot) {
    if (slot < 0 || slot >= num_slots()) return false;
    uint16_t off, len;
    slot_entry(slot, off, len);
    if (len == 0) return false;  // already deleted
    // Tombstone the slot. We leave the bytes in place (no compaction).
    set_slot_entry(slot, off, 0);
    return true;
}

bool Page::is_slot_live(int slot) const {
    if (slot < 0 || slot >= num_slots()) return false;
    uint16_t off, len;
    slot_entry(slot, off, len);
    return len != 0;
}

}  // namespace minidb
