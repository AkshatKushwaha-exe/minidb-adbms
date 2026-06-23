// Exception hierarchy used throughout MiniDB.
//
// A single base class (DBException) lets the CLI catch everything in one place
// while still allowing specific layers to throw something meaningful.
#pragma once

#include <stdexcept>
#include <string>

namespace minidb {

// Base for every error the engine raises on purpose.
class DBException : public std::runtime_error {
public:
    explicit DBException(const std::string& msg) : std::runtime_error(msg) {}
};

// Thrown by the storage layer (bad page id, file I/O, page full, ...).
class StorageException : public DBException {
public:
    explicit StorageException(const std::string& msg) : DBException(msg) {}
};

// Thrown by the buffer pool when no frame can be evicted (all pages pinned).
class BufferPoolFullException : public StorageException {
public:
    explicit BufferPoolFullException(const std::string& msg)
        : StorageException(msg) {}
};

// Thrown by the parser/binder for malformed or unsupported SQL.
class SQLException : public DBException {
public:
    explicit SQLException(const std::string& msg) : DBException(msg) {}
};

// Thrown by the catalog (unknown table/column, duplicate table, ...).
class CatalogException : public DBException {
public:
    explicit CatalogException(const std::string& msg) : DBException(msg) {}
};

// Thrown when a transaction is aborted because of a deadlock. The transaction
// manager catches this, rolls back, and the CLI reports it to the user.
class DeadlockException : public DBException {
public:
    explicit DeadlockException(const std::string& msg) : DBException(msg) {}
};

// Thrown on a constraint violation (e.g. duplicate primary key).
class ConstraintException : public DBException {
public:
    explicit ConstraintException(const std::string& msg) : DBException(msg) {}
};

}  // namespace minidb
