#pragma once

#include <sqlite3.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// Builds a database in the exact schema-1 layout that Milestones 3-5 wrote (no `roots` table, no
// `documents.root_id`, schema_version = '1', optionally the single-directory `indexed_root` binding
// from Milestone 5), so migration tests run against the real old format rather than a lookalike.
struct Schema1Document {
    std::string path;
    std::vector<std::pair<std::string, int>> terms;  // term, frequency
};

inline bool createSchema1Database(const std::filesystem::path& file, const std::vector<Schema1Document>& documents,
                                  const std::string& indexedRoot = "") {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(file.u8string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    std::string sql = R"sql(
        PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL;
        CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
        CREATE TABLE documents (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, size_bytes INTEGER NOT NULL,
            mtime_ns INTEGER NOT NULL, token_count INTEGER NOT NULL, indexed_at INTEGER NOT NULL);
        CREATE TABLE terms (id INTEGER PRIMARY KEY, term TEXT NOT NULL UNIQUE);
        CREATE TABLE postings (term_id INTEGER NOT NULL REFERENCES terms(id),
            doc_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE, frequency INTEGER NOT NULL,
            PRIMARY KEY (term_id, doc_id)) WITHOUT ROWID;
        CREATE INDEX postings_by_doc ON postings(doc_id);
        INSERT INTO meta(key, value) VALUES ('schema_version', '1');
    )sql";
    if (!indexedRoot.empty()) {
        sql += "INSERT INTO meta(key, value) VALUES ('indexed_root', '" + indexedRoot + "');";
    }
    int docId = 0;
    for (const Schema1Document& doc : documents) {
        int tokens = 0;
        for (const auto& t : doc.terms) tokens += t.second;
        sql += "INSERT INTO documents VALUES (" + std::to_string(++docId) + ", '" + doc.path + "', 10, 20, " +
               std::to_string(tokens) + ", 30);";
        for (const auto& t : doc.terms) {
            sql += "INSERT OR IGNORE INTO terms(term) VALUES ('" + t.first + "');";
            sql += "INSERT INTO postings SELECT id, " + std::to_string(docId) + ", " + std::to_string(t.second) +
                   " FROM terms WHERE term = '" + t.first + "';";
        }
    }
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return rc == SQLITE_OK;
}
