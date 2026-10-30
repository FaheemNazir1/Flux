#include "flux/roots.hpp"

#include "flux/path_utils.hpp"

#include <algorithm>
#include <chrono>
#include <system_error>

namespace fs = std::filesystem;

namespace flux {
namespace {

bool requireRegistry(const Database& db, std::string& error) {
    if (db.schemaVersion() < 2) {
        error = "this database was created by an older Flux (schema 1) and has no root registry; "
                "run a command that writes to it (for example 'flux root add <directory>') to upgrade it "
                "(a backup copy is made first)";
        return false;
    }
    return true;
}

bool equalForMatching(const std::string& a, const std::string& b) {
#ifdef _WIN32
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
#else
    return a == b;
#endif
}

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string describeRoots(const std::vector<RootInfo>& roots) {
    if (roots.empty()) return "none registered";
    std::string text;
    for (const RootInfo& r : roots) {
        text += (text.empty() ? "" : ", ") + r.path;
    }
    return text;
}

// Deletes terms no posting uses. Called inside the caller's transaction.
bool pruneTerms(Database& db) {
    return db.exec("DELETE FROM terms WHERE NOT EXISTS (SELECT 1 FROM postings p WHERE p.term_id = terms.id)");
}

}  // namespace

bool listRoots(Database& db, std::vector<RootInfo>& roots, std::string& error) {
    roots.clear();
    if (!requireRegistry(db, error)) return false;
    Statement statement;
    if (!statement.prepare(db, "SELECT r.id, r.path, r.added_at, "
                               "(SELECT COUNT(*) FROM documents d WHERE d.root_id = r.id) "
                               "FROM roots r ORDER BY r.path")) {
        error = "cannot read roots: " + db.lastError();
        return false;
    }
    Statement::Step step;
    while ((step = statement.step()) == Statement::Step::Row) {
        roots.push_back({statement.columnInt64(0), statement.columnText(1), statement.columnInt64(2),
                         statement.columnInt64(3)});
    }
    if (step == Statement::Step::Error) {
        error = "cannot read roots: " + db.lastError();
        return false;
    }
    return true;
}

bool countUnassignedDocuments(Database& db, std::int64_t& count, std::string& error) {
    count = 0;
    Statement statement;
    const char* sql = db.schemaVersion() >= 2 ? "SELECT COUNT(*) FROM documents WHERE root_id IS NULL"
                                              : "SELECT COUNT(*) FROM documents";  // schema 1: nothing has a root
    if (!statement.prepare(db, sql) || statement.step() != Statement::Step::Row) {
        error = "cannot count documents: " + db.lastError();
        return false;
    }
    count = statement.columnInt64(0);
    return true;
}

bool findRegisteredRoot(Database& db, const fs::path& userPath, RootInfo& root, bool& found, std::string& error) {
    found = false;
    std::vector<RootInfo> roots;
    if (!listRoots(db, roots, error)) return false;

    std::string key;
    fs::path canonical;
    std::string ignored;
    if (canonicalDirectory(userPath, canonical, ignored)) {
        key = rootKey(canonical);
    } else {
        key = rootKey(userPath);  // not resolvable (disconnected drive?): match the spelling as typed
    }
    for (const RootInfo& candidate : roots) {
        if (equalForMatching(candidate.path, key)) {
            root = candidate;
            found = true;
            return true;
        }
    }
    return true;
}

bool findRootContaining(Database& db, const std::string& pathKey, RootInfo& root, bool& found, std::string& error) {
    found = false;
    std::vector<RootInfo> roots;
    if (!listRoots(db, roots, error)) return false;
    for (const RootInfo& candidate : roots) {
        if (isPathUnder(pathKey, candidate.path)) {
            root = candidate;
            found = true;
            return true;
        }
    }
    return true;
}

bool addRoot(Database& db, const fs::path& directory, AddRootResult& result, std::string& error) {
    result = AddRootResult{};
    fs::path canonical;
    if (!canonicalDirectory(directory, canonical, error)) return false;
    const std::string key = rootKey(canonical);

    std::vector<RootInfo> roots;
    if (!listRoots(db, roots, error)) return false;
    for (const RootInfo& existing : roots) {
        if (existing.path == key) {
            result.root = existing;
            result.alreadyRegistered = true;
            return true;
        }
        if (isPathUnder(key, existing.path)) {
            error = "'" + key + "' is inside the registered root '" + existing.path +
                    "'; it is already covered. (Roots may not overlap.)";
            return false;
        }
        if (isPathUnder(existing.path, key)) {
            error = "'" + key + "' contains the registered root '" + existing.path +
                    "'. Remove that root first if you want to register the larger directory. (Roots may not overlap.)";
            return false;
        }
    }

    if (!db.begin()) {
        error = "cannot start transaction: " + db.lastError();
        return false;
    }
    bool ok = true;
    {
        Statement insert, adopt;
        ok = insert.prepare(db, "INSERT INTO roots(path, added_at) VALUES (?, ?)") && insert.bindText(1, key) &&
             insert.bindInt64(2, nowSeconds()) && insert.step() == Statement::Step::Done;
        if (ok) {
            result.root = {db.lastInsertRowId(), key, nowSeconds(), 0};
            ok = adopt.prepare(db, "UPDATE documents SET root_id = ?1 WHERE root_id IS NULL AND "
                                   "substr(path, 1, length(?2)) = ?2") &&
                 adopt.bindInt64(1, result.root.id) && adopt.bindText(2, rootPrefix(key)) &&
                 adopt.step() == Statement::Step::Done;
            if (ok) {
                result.adoptedDocuments = db.changes();
                result.root.documents = result.adoptedDocuments;
            }
        }
    }
    if (!ok || !db.commit()) {
        error = "cannot register root: " + db.lastError();
        db.rollback();
        return false;
    }
    return true;
}

bool removeRoot(Database& db, const fs::path& directory, RemoveRootResult& result, std::string& error) {
    result = RemoveRootResult{};
    bool found = false;
    if (!findRegisteredRoot(db, directory, result.root, found, error)) return false;
    if (!found) {
        std::vector<RootInfo> roots;
        std::string ignored;
        listRoots(db, roots, ignored);
        error = "'" + directory.string() + "' is not a registered root (registered: " + describeRoots(roots) + ")";
        return false;
    }

    if (!db.begin()) {
        error = "cannot start transaction: " + db.lastError();
        return false;
    }
    bool ok = true;
    {
        Statement count, remove;
        ok = count.prepare(db, "SELECT COUNT(*) FROM documents WHERE root_id = ?") &&
             count.bindInt64(1, result.root.id) && count.step() == Statement::Step::Row;
        if (ok) {
            result.removedDocuments = count.columnInt64(0);
            // ON DELETE CASCADE removes this root's documents, and their postings in turn.
            ok = remove.prepare(db, "DELETE FROM roots WHERE id = ?") && remove.bindInt64(1, result.root.id) &&
                 remove.step() == Statement::Step::Done;
            Statement forget;  // its "last synced" stamp goes with it
            ok = ok && forget.prepare(db, "DELETE FROM meta WHERE key = ?") &&
                 forget.bindText(1, "last_sync:" + result.root.path) && forget.step() == Statement::Step::Done;
        }
    }
    ok = ok && pruneTerms(db) && db.commit();
    if (!ok) {
        error = "cannot remove root (nothing was changed): " + db.lastError();
        db.rollback();
        return false;
    }
    return true;
}

}  // namespace flux
