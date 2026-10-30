#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace flux {

// Version 2 adds the `roots` table and `documents.root_id` (multiple registered roots).
// Version 1 databases are still readable; opening one for writing migrates it (see Database::open).
constexpr int kSchemaVersion = 2;
constexpr int kOldestReadableSchemaVersion = 1;

// Schema 1 / Milestone 5 `meta` key that bound a database to a single directory. The migration
// turns it into the first row of `roots`; nothing writes it any more.
constexpr const char* kLegacyIndexedRootKey = "indexed_root";

enum class OpenMode {
    ReadWrite,  // creates the file and schema if missing
    ReadOnly,   // the file must already exist and hold a Flux schema
};

// RAII wrapper around a SQLite connection. A connection must only be used by
// one thread at a time; Flux gives it to the IndexWriter thread while indexing.
class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Opens (and for ReadWrite, initializes) a Flux database. ":memory:" is allowed.
    // Fails if the file is not a Flux database or has an unsupported schema version.
    //
    // A schema-1 database opened ReadOnly stays schema 1 (search still works). Opened ReadWrite
    // it is upgraded to schema 2: for a file, a consistent copy is first written next to it
    // (`<name>.v1.bak`, never overwriting an existing file) and the upgrade then runs in one
    // transaction; if the copy cannot be made the database is left untouched and open fails.
    bool open(const std::filesystem::path& file, OpenMode mode, std::string& error);

    bool isOpen() const { return db_ != nullptr; }

    // 1 or 2 once open (see kSchemaVersion).
    int schemaVersion() const { return schemaVersion_; }

    // Set when open() upgraded the file from schema 1: where the backup copy was written.
    const std::filesystem::path& upgradeBackupPath() const { return upgradeBackup_; }
    bool wasUpgraded() const { return upgraded_; }

    // The `content_status` table (why each file is or is not searchable by content). Databases made by
    // earlier versions get it the first time they are opened for writing: a safety copy
    // (`<name>.pre-content.bak`, never overwriting an existing file) is made, then the table is created in
    // one transaction; nothing existing is altered. Opened read-only, an older database simply has no table.
    bool hasContentStatus() const { return hasContentStatus_; }
    bool createdContentStatus() const { return createdContentStatus_; }
    const std::filesystem::path& contentBackupPath() const { return contentBackup_; }

    // Writes a consistent copy of the whole database to `destination` using SQLite's online
    // backup API. `destination` must not exist.
    bool backupTo(const std::filesystem::path& destination, std::string& error);

    // Runs one or more statements that return no rows.
    bool exec(const std::string& sql);
    bool begin() { return exec("BEGIN"); }
    bool commit() { return exec("COMMIT"); }
    void rollback() { exec("ROLLBACK"); }  // best effort; harmless if no transaction is open

    // Reads a row of the `meta` table. `found` tells whether the key exists.
    // Returns false only if the query itself fails.
    bool getMeta(const std::string& key, std::string& value, bool& found);

    std::string lastError() const;
    std::int64_t lastInsertRowId() const;
    // Rows changed by the most recent INSERT/UPDATE/DELETE statement (cascades not included).
    int changes() const;
    sqlite3* handle() const { return db_; }

private:
    bool ensureSchema(OpenMode mode, std::string& error);
    bool upgradeFromV1(std::string& error);
    bool ensureContentStatus(OpenMode mode, std::string& error);

    sqlite3* db_ = nullptr;
    std::filesystem::path file_;
    int schemaVersion_ = 0;
    bool hasContentStatus_ = false;
    bool createdContentStatus_ = false;
    std::filesystem::path contentBackup_;
    bool upgraded_ = false;
    std::filesystem::path upgradeBackup_;
};

// RAII wrapper around a prepared statement. The Database must outlive it.
// Parameter and column indices follow SQLite: parameters start at 1, columns at 0.
class Statement {
public:
    enum class Step { Row, Done, Error };

    Statement() = default;
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    // On failure the reason is available from db.lastError().
    bool prepare(Database& db, const std::string& sql);

    bool bindText(int index, std::string_view text);
    bool bindInt64(int index, std::int64_t value);
    bool bindNull(int index);

    Step step();
    // Makes the statement runnable again and clears its bindings.
    bool reset();

    std::string columnText(int column) const;
    std::int64_t columnInt64(int column) const;

private:
    void finalize();

    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace flux
