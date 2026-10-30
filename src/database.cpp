#include "flux/database.hpp"

#include "flux/path_utils.hpp"

#include <sqlite3.h>

#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace flux {
namespace {

constexpr int kBusyTimeoutMs = 5000;

// Page cache for read-write connections (the indexing path), in KiB; memory is only
// used as pages are touched. SQLite's default of 2 MiB is far smaller than the
// working set of a batch of random-order posting inserts, so dirty pages spilled to
// the WAL and were re-read. Measured on the 'medium' benchmark dataset (see
// benchmarks/README.md): 18.6 -> 6.1 microseconds per posting at 2000 documents.
constexpr int kWriteCacheKiB = 64 * 1024;

const char* const kSchemaSql = R"sql(
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE roots (
    id       INTEGER PRIMARY KEY,
    path     TEXT    NOT NULL UNIQUE,
    added_at INTEGER NOT NULL
);
CREATE TABLE documents (
    id          INTEGER PRIMARY KEY,
    path        TEXT    NOT NULL UNIQUE,
    size_bytes  INTEGER NOT NULL,
    mtime_ns    INTEGER NOT NULL,
    token_count INTEGER NOT NULL,
    indexed_at  INTEGER NOT NULL,
    root_id     INTEGER REFERENCES roots(id) ON DELETE CASCADE
);
CREATE INDEX documents_by_root ON documents(root_id);
CREATE TABLE terms (
    id   INTEGER PRIMARY KEY,
    term TEXT NOT NULL UNIQUE
);
CREATE TABLE postings (
    term_id   INTEGER NOT NULL REFERENCES terms(id),
    doc_id    INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
    frequency INTEGER NOT NULL,
    PRIMARY KEY (term_id, doc_id)
) WITHOUT ROWID;
CREATE INDEX postings_by_doc ON postings(doc_id);
INSERT INTO meta(key, value) VALUES ('schema_version', '2');
)sql" ;

// Added after schema 2 shipped, without changing the schema version: older Flux versions keep working
// on a database that has it (they ignore the table, and deleting a document cascades to its row).
// It records WHY a file is or is not searchable by content (see extract.hpp).
const char* const kContentStatusSql = R"sql(
CREATE TABLE IF NOT EXISTS content_status (
    doc_id    INTEGER PRIMARY KEY REFERENCES documents(id) ON DELETE CASCADE,
    status    INTEGER NOT NULL,
    extractor INTEGER NOT NULL,
    detail    TEXT    NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS content_status_by_status ON content_status(status);
)sql";

}  // namespace

Database::~Database() {
    // _v2 defers the close if a statement is still alive, instead of leaking.
    sqlite3_close_v2(db_);
}

bool Database::open(const fs::path& file, OpenMode mode, std::string& error) {
    if (db_ != nullptr) {
        error = "database is already open";
        return false;
    }
    const std::string name = file.u8string();  // SQLite expects UTF-8
    file_ = file;
    schemaVersion_ = 0;
    upgraded_ = false;
    upgradeBackup_.clear();
    contentBackup_.clear();
    hasContentStatus_ = false;
    createdContentStatus_ = false;
    if (mode == OpenMode::ReadOnly && name != ":memory:") {
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            error = "database file not found: " + name;
            return false;
        }
    }

    const int flags = mode == OpenMode::ReadOnly
                          ? SQLITE_OPEN_READONLY
                          : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    sqlite3* handle = nullptr;
    const int rc = sqlite3_open_v2(name.c_str(), &handle, flags, nullptr);
    if (rc != SQLITE_OK) {
        error = "cannot open database " + name + ": " +
                (handle != nullptr ? sqlite3_errmsg(handle) : sqlite3_errstr(rc));
        sqlite3_close_v2(handle);
        return false;
    }
    db_ = handle;
    sqlite3_busy_timeout(db_, kBusyTimeoutMs);

    if (mode == OpenMode::ReadWrite) {
        if (!exec("PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; "
                  "PRAGMA cache_size = -" + std::to_string(kWriteCacheKiB) + ";")) {
            error = "cannot configure database " + name + ": " + lastError();
            sqlite3_close_v2(db_);
            db_ = nullptr;
            return false;
        }
    }
    if (!ensureSchema(mode, error)) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
        return false;
    }
    return true;
}

bool Database::ensureSchema(OpenMode mode, std::string& error) {
    Statement tables;
    if (!tables.prepare(*this,
                        "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'")) {
        error = "cannot read database: " + lastError();
        return false;
    }
    bool hasMeta = false;
    bool hasAnyTable = false;
    Statement::Step step;
    while ((step = tables.step()) == Statement::Step::Row) {
        hasAnyTable = true;
        hasMeta = hasMeta || tables.columnText(0) == "meta";
    }
    if (step == Statement::Step::Error) {
        error = "cannot read database: " + lastError();
        return false;
    }

    if (hasMeta) {
        std::string found;
        {
            Statement version;  // finalized before any upgrade below
            if (!version.prepare(*this, "SELECT value FROM meta WHERE key = 'schema_version'")) {
                error = "not a Flux database: " + lastError();
                return false;
            }
            const Statement::Step s = version.step();
            if (s == Statement::Step::Error) {
                error = "cannot read schema version: " + lastError();
                return false;
            }
            found = s == Statement::Step::Row ? version.columnText(0) : "(missing)";
        }
        if (found == std::to_string(kSchemaVersion)) {
            schemaVersion_ = kSchemaVersion;
            return ensureContentStatus(mode, error);
        }
        if (found == "1") {
            if (mode == OpenMode::ReadOnly) {
                schemaVersion_ = 1;  // searchable as it is; only a writing open upgrades it
                return true;
            }
            return upgradeFromV1(error) && ensureContentStatus(mode, error);
        }
        error = "unsupported database schema version " + found + " (this Flux reads versions " +
                std::to_string(kOldestReadableSchemaVersion) + " to " + std::to_string(kSchemaVersion) + ")";
        return false;
    }

    if (hasAnyTable) {
        error = "file is a SQLite database but not a Flux index";
        return false;
    }
    if (mode == OpenMode::ReadOnly) {
        error = "database is empty or not a Flux index";
        return false;
    }

    // Create the schema atomically so a crash cannot leave a half-built database.
    if (!begin() || !exec(kSchemaSql) || !exec(kContentStatusSql) || !commit()) {
        error = "cannot create schema: " + lastError();
        rollback();
        return false;
    }
    schemaVersion_ = kSchemaVersion;
    hasContentStatus_ = true;
    return true;
}

bool Database::ensureContentStatus(OpenMode mode, std::string& error) {
    hasContentStatus_ = false;
    {
        Statement probe;
        if (!probe.prepare(*this, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'content_status'")) {
            error = "cannot read database: " + lastError();
            return false;
        }
        const Statement::Step step = probe.step();
        if (step == Statement::Step::Error) {
            error = "cannot read database: " + lastError();
            return false;
        }
        if (step == Statement::Step::Row) {
            hasContentStatus_ = true;
            return true;
        }
    }
    if (mode == OpenMode::ReadOnly) return true;  // searchable as it is; only a writing open adds the table

    // A database that predates content extraction: copy it first (never overwriting a copy), then add the
    // table in one transaction. Nothing existing is altered, so the only thing a failure can leave is the copy.
    const std::string name = file_.u8string();
    std::int64_t documents = 0;
    {
        Statement count;
        if (count.prepare(*this, "SELECT count(*) FROM documents") && count.step() == Statement::Step::Row) documents = count.columnInt64(0);
    }
    if (name != ":memory:" && documents > 0 && !upgraded_) {
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(file_, ec);
        const fs::space_info space = fs::space(file_.has_parent_path() ? file_.parent_path() : fs::path("."), ec);
        if (!ec && space.available < size + size / 4) {
            error = "cannot add content-extraction tables to " + name + ": not enough free disk space for a safety copy of the database (" +
                    std::to_string(size / (1024 * 1024)) + " MiB needed). The database has not been changed.";
            return false;
        }
        fs::path backup;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            backup = fs::path(name + ".pre-content.bak" + (attempt == 0 ? "" : "." + std::to_string(attempt)));
            if (!fs::exists(backup, ec)) break;
        }
        if (!backupTo(backup, error)) {
            error = "cannot add content-extraction tables to " + name + " (it has not been changed): " + error;
            return false;
        }
        contentBackup_ = backup;
    }
    if (!begin() || !exec(kContentStatusSql) || !commit()) {
        error = "adding the content-status table failed and was rolled back: " + lastError();
        rollback();
        return false;
    }
    hasContentStatus_ = true;
    createdContentStatus_ = true;
    return true;
}

bool Database::backupTo(const fs::path& destination, std::string& error) {
    std::error_code ec;
    if (fs::exists(destination, ec)) {
        error = "backup target already exists: " + destination.string();
        return false;
    }
    sqlite3* target = nullptr;
    if (sqlite3_open_v2(destination.u8string().c_str(), &target,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        error = "cannot create backup " + destination.string() + ": " +
                (target != nullptr ? sqlite3_errmsg(target) : "unknown error");
        sqlite3_close_v2(target);
        return false;
    }
    bool ok = false;
    if (sqlite3_backup* backup = sqlite3_backup_init(target, "main", db_, "main")) {
        const int rc = sqlite3_backup_step(backup, -1);  // the whole database in one step
        ok = sqlite3_backup_finish(backup) == SQLITE_OK && rc == SQLITE_DONE;
    }
    if (!ok) {
        error = "backup to " + destination.string() + " failed: " + sqlite3_errmsg(target);
    }
    sqlite3_close_v2(target);
    if (!ok) {
        fs::remove(destination, ec);  // never leave a half-written backup behind
    }
    return ok;
}

bool Database::upgradeFromV1(std::string& error) {
    // 1. A consistent copy first. Without it the file is not touched.
    const std::string name = file_.u8string();
    if (name != ":memory:") {
        fs::path backup;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            backup = fs::path(name + ".v1.bak" + (attempt == 0 ? "" : "." + std::to_string(attempt)));
            std::error_code ec;
            if (!fs::exists(backup, ec)) break;
        }
        if (!backupTo(backup, error)) {
            error = "cannot upgrade database " + name + " from schema 1 to 2 (it has not been changed): " + error;
            return false;
        }
        upgradeBackup_ = backup;
    }

    // 2. One transaction: either the whole upgrade happens or none of it.
    std::string boundRoot;
    bool bound = false;
    if (!getMeta(kLegacyIndexedRootKey, boundRoot, bound)) {
        error = "cannot read database: " + lastError();
        return false;
    }
    bool ok = begin() &&
              exec("CREATE TABLE roots (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, added_at INTEGER NOT NULL)") &&
              exec("ALTER TABLE documents ADD COLUMN root_id INTEGER REFERENCES roots(id) ON DELETE CASCADE") &&
              exec("CREATE INDEX documents_by_root ON documents(root_id)");
    if (ok && bound) {
        // The single directory Milestone 5 had bound the database to becomes the first root, and the
        // documents under it belong to it. Documents outside it (there should be none) stay unassigned.
        Statement insertRoot, assign;
        const std::string prefix = rootPrefix(boundRoot);
        ok = insertRoot.prepare(*this, "INSERT INTO roots(path, added_at) VALUES (?, strftime('%s','now'))") &&
             insertRoot.bindText(1, boundRoot) && insertRoot.step() == Statement::Step::Done;
        if (ok) {
            const std::int64_t rootId = lastInsertRowId();
            ok = assign.prepare(*this, "UPDATE documents SET root_id = ?1 WHERE root_id IS NULL AND "
                                       "substr(path, 1, length(?2)) = ?2") &&
                 assign.bindInt64(1, rootId) && assign.bindText(2, prefix) &&
                 assign.step() == Statement::Step::Done;
        }
    }
    ok = ok && exec("DELETE FROM meta WHERE key = 'indexed_root'") &&
         exec("UPDATE meta SET value = '2' WHERE key = 'schema_version'") && commit();
    if (!ok) {
        error = "database upgrade from schema 1 to 2 failed and was rolled back: " + lastError();
        rollback();
        return false;
    }
    schemaVersion_ = kSchemaVersion;
    upgraded_ = true;
    return true;
}

bool Database::exec(const std::string& sql) {
    return sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::string Database::lastError() const {
    return db_ != nullptr ? sqlite3_errmsg(db_) : "database is not open";
}

bool Database::getMeta(const std::string& key, std::string& value, bool& found) {
    found = false;
    Statement statement;
    if (!statement.prepare(*this, "SELECT value FROM meta WHERE key = ?") || !statement.bindText(1, key)) {
        return false;
    }
    const Statement::Step step = statement.step();
    if (step == Statement::Step::Error) {
        return false;
    }
    if (step == Statement::Step::Row) {
        value = statement.columnText(0);
        found = true;
    }
    return true;
}

std::int64_t Database::lastInsertRowId() const {
    return sqlite3_last_insert_rowid(db_);
}

int Database::changes() const {
    return sqlite3_changes(db_);
}

Statement::~Statement() {
    finalize();
}

Statement::Statement(Statement&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        finalize();
        db_ = std::exchange(other.db_, nullptr);
        stmt_ = std::exchange(other.stmt_, nullptr);
    }
    return *this;
}

void Statement::finalize() {
    sqlite3_finalize(stmt_);
    stmt_ = nullptr;
}

bool Statement::prepare(Database& db, const std::string& sql) {
    finalize();
    db_ = db.handle();
    return sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()), &stmt_, nullptr) ==
           SQLITE_OK;
}

bool Statement::bindText(int index, std::string_view text) {
    return sqlite3_bind_text(stmt_, index, text.data(), static_cast<int>(text.size()),
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

bool Statement::bindInt64(int index, std::int64_t value) {
    return sqlite3_bind_int64(stmt_, index, value) == SQLITE_OK;
}

bool Statement::bindNull(int index) {
    return sqlite3_bind_null(stmt_, index) == SQLITE_OK;
}

Statement::Step Statement::step() {
    switch (sqlite3_step(stmt_)) {
        case SQLITE_ROW: return Step::Row;
        case SQLITE_DONE: return Step::Done;
        default: return Step::Error;
    }
}

bool Statement::reset() {
    // sqlite3_reset returns the error of the previous step; that was already
    // reported by step(), so only the clear-bindings result matters here.
    sqlite3_reset(stmt_);
    return sqlite3_clear_bindings(stmt_) == SQLITE_OK;
}

std::string Statement::columnText(int column) const {
    const unsigned char* text = sqlite3_column_text(stmt_, column);
    const int size = sqlite3_column_bytes(stmt_, column);
    return text != nullptr ? std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(size))
                           : std::string();
}

std::int64_t Statement::columnInt64(int column) const {
    return sqlite3_column_int64(stmt_, column);
}

}  // namespace flux
