#include "minidb/storage/disk_manager.h"

#include "minidb/exceptions.h"

namespace minidb {

DiskManager::DiskManager(const std::string& path) : path_(path) {
    // Open for read+write in binary mode. The file may not exist yet, so we
    // first try to open it and, if that fails, create it.
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        // Create the file.
        std::ofstream create(path_, std::ios::out | std::ios::binary);
        create.close();
        file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    }
    if (!file_.is_open()) {
        throw StorageException("DiskManager: cannot open file " + path_);
    }
    // Determine how many whole pages already exist in the file.
    file_.seekg(0, std::ios::end);
    std::streamoff size = file_.tellg();
    if (size < 0) size = 0;
    num_pages_ = static_cast<page_id_t>(size / PAGE_SIZE);
}

DiskManager::~DiskManager() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

page_id_t DiskManager::allocate_page() {
    page_id_t id = num_pages_;
    // Physically extend the file with a zero-filled page so that later reads
    // of this id succeed even before anything meaningful is written.
    std::vector<uint8_t> empty(PAGE_SIZE, 0);
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(id) * PAGE_SIZE, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(empty.data()), PAGE_SIZE);
    file_.flush();
    if (!file_) throw StorageException("DiskManager: failed to allocate page");
    ++num_pages_;
    ++writes_;
    return id;
}

void DiskManager::read_page(page_id_t id, std::vector<uint8_t>& out) {
    if (id < 0 || id >= num_pages_) {
        throw StorageException("DiskManager: read_page out of range");
    }
    out.assign(PAGE_SIZE, 0);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(id) * PAGE_SIZE, std::ios::beg);
    file_.read(reinterpret_cast<char*>(out.data()), PAGE_SIZE);
    // A short read can happen if the page was allocated but never written; the
    // remainder is already zero-filled, so we just clear the stream state.
    file_.clear();
    ++reads_;
}

void DiskManager::write_page(page_id_t id, const std::vector<uint8_t>& bytes) {
    if (bytes.size() != PAGE_SIZE) {
        throw StorageException("DiskManager: write_page wrong buffer size");
    }
    if (id < 0 || id >= num_pages_) {
        throw StorageException("DiskManager: write_page out of range");
    }
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(id) * PAGE_SIZE, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(bytes.data()), PAGE_SIZE);
    file_.flush();  // push to the OS so a crash test sees the data
    if (!file_) throw StorageException("DiskManager: write failed");
    ++writes_;
}

}  // namespace minidb
