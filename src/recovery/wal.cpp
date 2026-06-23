#include "minidb/recovery/wal.h"

#include <cstring>

#include "minidb/exceptions.h"

namespace minidb {

namespace {
void put_i64(std::vector<uint8_t>& b, int64_t v) {
    uint8_t t[8];
    std::memcpy(t, &v, 8);
    b.insert(b.end(), t, t + 8);
}
void put_i32(std::vector<uint8_t>& b, int32_t v) {
    uint8_t t[4];
    std::memcpy(t, &v, 4);
    b.insert(b.end(), t, t + 4);
}
void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    uint8_t t[4];
    std::memcpy(t, &v, 4);
    b.insert(b.end(), t, t + 4);
}
}  // namespace

WAL::WAL(const std::string& path) : path_(path) {
    // Determine the next LSN from any existing log so we continue the sequence.
    std::vector<LogRecord> existing = read_all(path_);
    if (!existing.empty()) {
        next_lsn_ = existing.back().lsn + 1;
        flushed_lsn_ = existing.back().lsn;  // already on disk
    }
    // Append from here on.
    out_.open(path_, std::ios::out | std::ios::app | std::ios::binary);
    if (!out_.is_open()) {
        throw StorageException("WAL: cannot open log file " + path_);
    }
}

WAL::~WAL() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

lsn_t WAL::append(const LogRecord& rec_in) {
    LogRecord rec = rec_in;
    rec.lsn = next_lsn_++;

    std::vector<uint8_t> body;
    put_i64(body, rec.lsn);
    body.push_back(static_cast<uint8_t>(rec.type));
    put_i64(body, rec.txn);
    put_i32(body, rec.file_id);
    put_i32(body, rec.rid.page_id);
    put_i32(body, rec.rid.slot);
    put_u32(body, static_cast<uint32_t>(rec.image.size()));
    body.insert(body.end(), rec.image.begin(), rec.image.end());

    uint32_t len = static_cast<uint32_t>(body.size());
    out_.write(reinterpret_cast<const char*>(&len), 4);
    out_.write(reinterpret_cast<const char*>(body.data()), body.size());
    // Note: not flushed here -- durability happens on commit or when a page
    // flush forces it (write-ahead rule), keeping the common path fast.
    return rec.lsn;
}

lsn_t WAL::log_begin(txn_id_t txn) {
    LogRecord r;
    r.type = LogType::BEGIN;
    r.txn = txn;
    return append(r);
}

lsn_t WAL::log_commit(txn_id_t txn) {
    LogRecord r;
    r.type = LogType::COMMIT;
    r.txn = txn;
    lsn_t lsn = append(r);
    flush();  // a commit is not acknowledged until its log record is durable
    return lsn;
}

lsn_t WAL::log_abort(txn_id_t txn) {
    LogRecord r;
    r.type = LogType::ABORT;
    r.txn = txn;
    lsn_t lsn = append(r);
    flush();
    return lsn;
}

lsn_t WAL::log_checkpoint() {
    LogRecord r;
    r.type = LogType::CHECKPOINT;
    lsn_t lsn = append(r);
    flush();
    return lsn;
}

lsn_t WAL::log_insert(txn_id_t txn, int file_id, const RID& rid,
                      const std::vector<uint8_t>& after_image) {
    LogRecord r;
    r.type = LogType::INSERT;
    r.txn = txn;
    r.file_id = file_id;
    r.rid = rid;
    r.image = after_image;
    return append(r);
}

lsn_t WAL::log_delete(txn_id_t txn, int file_id, const RID& rid,
                      const std::vector<uint8_t>& before_image) {
    LogRecord r;
    r.type = LogType::DELETE;
    r.txn = txn;
    r.file_id = file_id;
    r.rid = rid;
    r.image = before_image;
    return append(r);
}

void WAL::flush() {
    out_.flush();
    flushed_lsn_ = next_lsn_ - 1;
}

void WAL::flush_to_lsn(lsn_t target) {
    if (target > flushed_lsn_) flush();
}

std::vector<LogRecord> WAL::read_all(const std::string& path) {
    std::vector<LogRecord> records;
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) return records;  // no log yet

    while (true) {
        uint32_t len = 0;
        in.read(reinterpret_cast<char*>(&len), 4);
        if (in.gcount() < 4) break;  // clean EOF
        std::vector<uint8_t> body(len);
        in.read(reinterpret_cast<char*>(body.data()), len);
        if (static_cast<uint32_t>(in.gcount()) < len) {
            // Torn record from a crash mid-write: ignore the partial tail.
            break;
        }
        // Parse the body.
        std::size_t pos = 0;
        auto get_i64 = [&](int64_t& v) {
            std::memcpy(&v, body.data() + pos, 8);
            pos += 8;
        };
        auto get_i32 = [&](int32_t& v) {
            std::memcpy(&v, body.data() + pos, 4);
            pos += 4;
        };
        auto get_u32 = [&](uint32_t& v) {
            std::memcpy(&v, body.data() + pos, 4);
            pos += 4;
        };

        LogRecord r;
        get_i64(r.lsn);
        r.type = static_cast<LogType>(body[pos++]);
        get_i64(r.txn);
        get_i32(r.file_id);
        get_i32(r.rid.page_id);
        get_i32(r.rid.slot);
        uint32_t img_len = 0;
        get_u32(img_len);
        r.image.assign(body.begin() + pos, body.begin() + pos + img_len);
        records.push_back(std::move(r));
    }
    return records;
}

}  // namespace minidb
