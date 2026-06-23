#include "minidb/recovery/recovery_manager.h"

#include <algorithm>
#include <unordered_set>

#include "minidb/recovery/wal.h"
#include "minidb/storage/page.h"

namespace minidb {

void RecoveryManager::ensure_page_exists(int file_id, page_id_t page_id) {
    while (bpool_->file_page_count(file_id) <= page_id) {
        page_id_t pid;
        bpool_->new_page(file_id, &pid);
        bpool_->unpin_page(file_id, pid, true);
    }
}

void RecoveryManager::redo_record(const LogRecord& r) {
    ensure_page_exists(r.file_id, r.rid.page_id);
    Page* p = bpool_->fetch_page(r.file_id, r.rid.page_id);
    // A page allocated but never flushed before the crash reads back all-zero;
    // re-initialise it into a valid empty page before touching it.
    if (!p->is_initialized()) *p = Page();

    // Page-LSN guard: only redo if the page does not already reflect this
    // change. This is what makes redo safe to run after a partial flush.
    if (p->lsn() < r.lsn) {
        if (r.type == LogType::INSERT) {
            p->insert_record_at(r.rid.slot, r.image);
        } else {  // DELETE
            p->delete_record(r.rid.slot);
        }
        p->set_lsn(r.lsn);
        bpool_->unpin_page(r.file_id, r.rid.page_id, /*dirty=*/true);
    } else {
        bpool_->unpin_page(r.file_id, r.rid.page_id, /*dirty=*/false);
    }
}

void RecoveryManager::undo_record(const LogRecord& r) {
    ensure_page_exists(r.file_id, r.rid.page_id);
    Page* p = bpool_->fetch_page(r.file_id, r.rid.page_id);
    if (!p->is_initialized()) *p = Page();

    if (r.type == LogType::INSERT) {
        // Undo an insert by tombstoning the slot (idempotent).
        p->delete_record(r.rid.slot);
    } else {  // DELETE
        // Undo a delete by putting the old bytes back (only if not present).
        if (!p->is_slot_live(r.rid.slot)) {
            p->insert_record_at(r.rid.slot, r.image);
        }
    }
    p->set_lsn(r.lsn);
    bpool_->unpin_page(r.file_id, r.rid.page_id, /*dirty=*/true);
}

RecoveryStats RecoveryManager::recover() {
    RecoveryStats stats;
    std::vector<LogRecord> log = WAL::read_all(wal_path_);
    if (log.empty()) return stats;

    // --- Phase 1: Analysis -- which transactions committed? -----------------
    std::unordered_set<txn_id_t> committed;
    std::unordered_set<txn_id_t> seen;
    for (const auto& r : log) {
        if (r.txn != INVALID_TXN_ID) seen.insert(r.txn);
        if (r.type == LogType::COMMIT) committed.insert(r.txn);
    }
    stats.committed_txns = static_cast<int>(committed.size());
    stats.loser_txns = static_cast<int>(seen.size() - committed.size());

    // --- Phase 2: Redo -- repeat history for all data-change records --------
    for (const auto& r : log) {
        if (r.type == LogType::INSERT || r.type == LogType::DELETE) {
            redo_record(r);
            ++stats.redone;
        }
    }

    // --- Phase 3: Undo -- roll back losers in reverse order -----------------
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        const LogRecord& r = *it;
        if ((r.type == LogType::INSERT || r.type == LogType::DELETE) &&
            committed.count(r.txn) == 0) {
            undo_record(r);
            ++stats.undone;
        }
    }

    // Make the recovered state durable.
    bpool_->flush_all();
    return stats;
}

}  // namespace minidb
