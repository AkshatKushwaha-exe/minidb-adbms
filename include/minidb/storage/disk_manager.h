// The disk manager owns one file on disk and reads/writes it one page at a
// time. It is the only component that touches the filesystem for table data.
//
// A page id is simply the page's index in the file: byte offset = id * PAGE_SIZE.
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "minidb/constants.h"

namespace minidb {

class DiskManager {
public:
    // Opens (creating if necessary) the file at `path`.
    explicit DiskManager(const std::string& path);
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // Grow the file by one page and return its id. The new page is zero-filled.
    page_id_t allocate_page();

    // Read page `id` into `out` (resized to PAGE_SIZE). Throws on bad id.
    void read_page(page_id_t id, std::vector<uint8_t>& out);

    // Write `bytes` (must be PAGE_SIZE) to page `id`, flushing to the OS.
    void write_page(page_id_t id, const std::vector<uint8_t>& bytes);

    // Number of pages currently allocated in the file.
    page_id_t num_pages() const { return num_pages_; }

    const std::string& path() const { return path_; }

    // Count of physical page reads/writes -- handy for benchmarks and to show
    // the buffer pool is actually saving I/O.
    uint64_t reads() const { return reads_; }
    uint64_t writes() const { return writes_; }

private:
    std::string path_;
    std::fstream file_;
    page_id_t num_pages_ = 0;
    uint64_t reads_ = 0;
    uint64_t writes_ = 0;
};

}  // namespace minidb
