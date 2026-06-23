#include "minidb/storage/heap_file.h"

#include "minidb/exceptions.h"

namespace minidb {

HeapFile::HeapFile(BufferPool* bpool, int file_id, ILogManager* log)
    : bpool_(bpool), file_id_(file_id), log_(log) {}

page_id_t HeapFile::page_count() const {
    // The DiskManager is the source of truth for the page count; the buffer
    // pool exposes it for the file we belong to.
    return bpool_->file_page_count(file_id_);
}

RID HeapFile::insert(const std::vector<uint8_t>& record, txn_id_t txn) {
    page_id_t n = page_count();

    // Fast path: try the last page (most likely to have room). This keeps bulk
    // inserts O(1) instead of scanning every page. The trade-off is that space
    // freed by deletes in earlier pages is not reused for new inserts -- a real
    // system tracks free space per page (PostgreSQL's FSM); we note this as
    // future work in the docs.
    if (n > 0) {
        page_id_t pid = n - 1;
        Page* p = bpool_->fetch_page(file_id_, pid);
        if (record.size() + Page::SLOT_SIZE <= p->free_space()) {
            int slot = static_cast<int>(p->num_slots());
            RID rid{pid, slot};
            lsn_t lsn = INVALID_LSN;
            if (log_) lsn = log_->log_insert(txn, file_id_, rid, record);
            int got = p->insert_record(record);
            if (got >= 0 && lsn != INVALID_LSN) p->set_lsn(lsn);
            bpool_->unpin_page(file_id_, pid, true);
            return RID{pid, got};
        }
        bpool_->unpin_page(file_id_, pid, false);
    }

    // Slow path: allocate a fresh page and insert there.
    page_id_t pid;
    Page* p = bpool_->new_page(file_id_, &pid);
    int slot = static_cast<int>(p->num_slots());
    RID rid{pid, slot};
    lsn_t lsn = INVALID_LSN;
    if (log_) lsn = log_->log_insert(txn, file_id_, rid, record);
    int got = p->insert_record(record);
    if (got < 0) {
        bpool_->unpin_page(file_id_, pid, false);
        throw StorageException("record too large to fit in an empty page");
    }
    if (lsn != INVALID_LSN) p->set_lsn(lsn);
    bpool_->unpin_page(file_id_, pid, true);
    return RID{pid, got};
}

void HeapFile::insert_at(const RID& rid, const std::vector<uint8_t>& record,
                         lsn_t page_lsn) {
    // Ensure all pages up to rid.page_id exist (recovery may run on a file that
    // is shorter than the log implies, though normally allocate_page persists).
    while (page_count() <= rid.page_id) {
        page_id_t pid;
        bpool_->new_page(file_id_, &pid);
        bpool_->unpin_page(file_id_, pid, true);
    }
    Page* p = bpool_->fetch_page(file_id_, rid.page_id);
    p->insert_record_at(rid.slot, record);
    p->set_lsn(page_lsn);
    bpool_->unpin_page(file_id_, rid.page_id, true);
}

bool HeapFile::get(const RID& rid, std::vector<uint8_t>& out) {
    if (rid.page_id < 0 || rid.page_id >= page_count()) return false;
    Page* p = bpool_->fetch_page(file_id_, rid.page_id);
    bool ok = p->get_record(rid.slot, out);
    bpool_->unpin_page(file_id_, rid.page_id, false);
    return ok;
}

bool HeapFile::remove(const RID& rid, txn_id_t txn) {
    if (rid.page_id < 0 || rid.page_id >= page_count()) return false;
    Page* p = bpool_->fetch_page(file_id_, rid.page_id);
    std::vector<uint8_t> before;
    if (!p->get_record(rid.slot, before)) {
        bpool_->unpin_page(file_id_, rid.page_id, false);
        return false;  // already deleted / never existed
    }
    lsn_t lsn = INVALID_LSN;
    if (log_) lsn = log_->log_delete(txn, file_id_, rid, before);
    p->delete_record(rid.slot);
    if (lsn != INVALID_LSN) p->set_lsn(lsn);
    bpool_->unpin_page(file_id_, rid.page_id, true);
    return true;
}

void HeapFile::remove_at(const RID& rid, lsn_t page_lsn) {
    if (rid.page_id < 0 || rid.page_id >= page_count()) return;
    Page* p = bpool_->fetch_page(file_id_, rid.page_id);
    p->delete_record(rid.slot);
    p->set_lsn(page_lsn);
    bpool_->unpin_page(file_id_, rid.page_id, true);
}

// --- iterator ---------------------------------------------------------------

HeapFile::Iterator::Iterator(HeapFile* hf, bool end)
    : hf_(hf), page_id_(0), slot_(-1), at_end_(end) {
    if (!end) advance_to_live();
}

void HeapFile::Iterator::advance_to_live() {
    page_id_t n = hf_->page_count();
    while (page_id_ < n) {
        Page* p = hf_->bpool_->fetch_page(hf_->file_id_, page_id_);
        int ns = static_cast<int>(p->num_slots());
        for (int s = slot_ + 1; s < ns; ++s) {
            if (p->get_record(s, cur_rec_)) {
                slot_ = s;
                cur_rid_ = RID{page_id_, s};
                hf_->bpool_->unpin_page(hf_->file_id_, page_id_, false);
                return;
            }
        }
        hf_->bpool_->unpin_page(hf_->file_id_, page_id_, false);
        page_id_++;
        slot_ = -1;
    }
    at_end_ = true;
}

bool HeapFile::Iterator::operator!=(const Iterator& o) const {
    if (at_end_ && o.at_end_) return false;
    if (at_end_ != o.at_end_) return true;
    return cur_rid_ != o.cur_rid_;
}

HeapFile::Iterator& HeapFile::Iterator::operator++() {
    advance_to_live();
    return *this;
}

std::pair<RID, std::vector<uint8_t>> HeapFile::Iterator::operator*() const {
    return {cur_rid_, cur_rec_};
}

}  // namespace minidb
