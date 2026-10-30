#include "flux/service.hpp"

#include "flux/database.hpp"
#include "flux/options.hpp"
#include "flux/path_utils.hpp"
#include "flux/tokenizer.hpp"

#include <chrono>
#include <system_error>

namespace fs = std::filesystem;

namespace flux {
namespace {

bool isAvailable(const std::string& storedPath) {
    std::error_code ec;
    return fs::is_directory(fs::u8path(storedPath), ec);
}

std::int64_t scalar(Database& db, const char* sql) {
    Statement s;
    return s.prepare(db, sql) && s.step() == Statement::Step::Row ? s.columnInt64(0) : -1;
}

bool openForWrite(const DatabaseLocation& location, Database& db, std::string& error, std::string* notice) {
    if (!prepareDatabaseDirectory(location, error)) return false;
    if (!db.open(location.path, OpenMode::ReadWrite, error)) return false;
    if (db.wasUpgraded() && notice != nullptr) {
        *notice = "The database was created by an older Flux and has been upgraded; a copy of the previous "
                  "version was saved to " + db.upgradeBackupPath().u8string();
    } else if (db.createdContentStatus() && notice != nullptr) {
        *notice = "Flux added the table that records why a file's content is or is not searchable. Nothing else in the "
                  "database changed";
        if (!db.contentBackupPath().empty()) *notice += "; a copy made beforehand was saved to " + db.contentBackupPath().u8string();
    }
    return true;
}

SyncOptions toSyncOptions(const SyncRunOptions& run, const std::string& rootPath) {
    SyncOptions options;
    options.threads = run.threads != 0 ? run.threads : defaultThreadCount();
    options.scanOptions.skipSystemDirectories = !run.includeSystemFolders;
    options.scanOptions.includeFileNames = run.indexFileNames;
    options.scanOptions.excludedFolders = run.excludedFolders;
    options.scanOptions.pause = run.pause;
    options.cancel = run.cancel;
    options.contentRefresh = run.contentRefresh;
    if (run.progress) {
        options.progress = [progress = run.progress, rootPath](const char* stage, std::size_t done, std::size_t total) {
            progress(rootPath, stage, done, total);
        };
    }
    return options;
}

bool isCancelled(const std::string& error) {
    return error.rfind("cancelled", 0) == 0;
}

}  // namespace

bool FluxService::loadDashboard(Dashboard& dashboard, std::string& error) const {
    dashboard = Dashboard{};
    dashboard.databasePath = location_.path;
    dashboard.source = location_.source;
    std::error_code ec;
    if (!fs::exists(location_.path, ec)) {
        return true;  // nothing indexed yet
    }
    dashboard.databaseExists = true;
    dashboard.sizeBytes = fs::file_size(location_.path, ec);

    Database db;
    if (!db.open(location_.path, OpenMode::ReadOnly, error)) return false;
    dashboard.schemaVersion = db.schemaVersion();
    dashboard.documents = scalar(db, "SELECT COUNT(*) FROM documents");
    dashboard.terms = scalar(db, "SELECT COUNT(*) FROM terms");
    {
        std::string ignored;
        dashboard.contentKnown = queryContentCounts(db, 0, dashboard.content, nullptr, ignored);
    }
    if (db.schemaVersion() < 2) {
        dashboard.unassignedDocuments = dashboard.documents;  // an old database has no roots yet
        return true;
    }
    std::vector<RootInfo> roots;
    if (!listRoots(db, roots, error)) return false;
    for (RootInfo& info : roots) {
        RootStatus status;
        status.available = isAvailable(info.path);
        status.info = std::move(info);
        dashboard.roots.push_back(std::move(status));
    }
    if (!countUnassignedDocuments(db, dashboard.unassignedDocuments, error)) return false;
    return true;
}

namespace {

// "D:/" or "D:/Projects" -> 'D'; other spellings (UNC paths, ...) -> 0.
char driveLetterOf(const std::string& storedPath) {
    if (storedPath.size() >= 2 && storedPath[1] == ':' &&
        ((storedPath[0] >= 'A' && storedPath[0] <= 'Z') || (storedPath[0] >= 'a' && storedPath[0] <= 'z'))) {
        return static_cast<char>(storedPath[0] >= 'a' ? storedPath[0] - 'a' + 'A' : storedPath[0]);
    }
    return 0;
}

std::int64_t lastSyncOf(Database& db, const std::string& rootPath) {
    Statement statement;
    if (!statement.prepare(db, "SELECT value FROM meta WHERE key = ?") ||
        !statement.bindText(1, std::string(kLastSyncKeyPrefix) + rootPath) || statement.step() != Statement::Step::Row) {
        return 0;
    }
    try {
        return std::stoll(statement.columnText(0));
    } catch (const std::exception&) {
        return 0;
    }
}

}  // namespace

bool FluxService::loadDrives(DriveView& view, std::string& error) const {
    view = DriveView{};
    std::vector<DriveInfo> drives = discoverDrives();

    std::vector<RootInfo> roots;
    std::vector<std::int64_t> syncTimes;
    std::error_code ec;
    if (fs::exists(location_.path, ec)) {
        Database db;
        if (!db.open(location_.path, OpenMode::ReadOnly, error)) return false;
        if (db.schemaVersion() >= 2) {
            if (!listRoots(db, roots, error)) return false;
            for (const RootInfo& root : roots) syncTimes.push_back(lastSyncOf(db, root.path));
        }
    }

    std::vector<bool> placed(roots.size(), false);
    for (DriveInfo& info : drives) {
        DriveStatus status;
        for (std::size_t i = 0; i < roots.size(); ++i) {
            if (driveLetterOf(roots[i].path) != info.letter) continue;
            placed[i] = true;
            status.roots.push_back(roots[i].path);
            status.files += roots[i].documents;
            status.lastSyncUnix = std::max(status.lastSyncUnix, syncTimes[i]);
            if (roots[i].path.size() <= 3) status.indexed = true;  // "D:/"
            else status.partlyIndexed = true;
        }
        if (status.indexed) status.partlyIndexed = false;
        status.info = std::move(info);
        view.drives.push_back(std::move(status));
    }
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (placed[i]) continue;
        RootStatus status;
        status.info = roots[i];
        status.available = isAvailable(roots[i].path);  // e.g. a UNC share reachable without a drive letter
        if (!status.available || driveLetterOf(roots[i].path) != 0) view.notConnected.push_back(std::move(status));
    }
    return true;
}

SearchOutcome FluxService::search(const SearchRequest& request) const {
    SearchOutcome outcome;
    const auto started = std::chrono::steady_clock::now();
    auto finish = [&](ServiceError kind, std::string message) {
        outcome.kind = kind;
        outcome.error = std::move(message);
        outcome.ok = kind == ServiceError::None;
        outcome.elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return outcome;
    };

    std::error_code ec;
    if (!fs::exists(location_.path, ec)) {
        return finish(ServiceError::NoIndex,
                      "There is no index yet. Add a folder and synchronize it to start searching.");
    }
    Database db;
    std::string error;
    if (!db.open(location_.path, OpenMode::ReadOnly, error)) return finish(ServiceError::Failed, error);

    std::int64_t rootId = 0;
    if (!request.rootPath.empty()) {
        RootInfo root;
        bool found = false;
        if (!findRegisteredRoot(db, fs::u8path(request.rootPath), root, found, error)) {
            return finish(ServiceError::Failed, error);
        }
        if (!found) {
            return finish(ServiceError::BadRequest, "'" + request.rootPath + "' is not a registered folder.");
        }
        rootId = root.id;
    }
    if (!searchFiles(db, request.query, request.limit, outcome.hits, error, rootId)) {
        if (error == "query has no searchable terms") {
            return finish(ServiceError::BadRequest, "Type one or more words to search for.");
        }
        return finish(ServiceError::Failed, error);
    }
    if (request.snippets) addSnippets(outcome.hits, request.query);
    addContentNotes(db, outcome.hits);
    if (outcome.hits.empty() && scalar(db, "SELECT COUNT(*) FROM documents") == 0) {
        return finish(ServiceError::EmptyIndex,
                      "The index is empty. Synchronize a folder to read its files into the index.");
    }
    return finish(ServiceError::None, "");
}

bool FluxService::addRoot(const fs::path& directory, AddRootResult& result, std::string& error,
                          std::string* notice) const {
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) {
        error = "folder not found: " + directory.u8string() +
                " (if it is on a removable or network drive, is the drive connected?)";
        return false;
    }
    Database db;
    if (!openForWrite(location_, db, error, notice)) return false;
    return flux::addRoot(db, directory, result, error);
}

bool FluxService::removeRoot(const fs::path& directory, RemoveRootResult& result, std::string& error) const {
    Database db;
    if (!openForWrite(location_, db, error, nullptr)) return false;
    return flux::removeRoot(db, directory, result, error);
}

bool FluxService::syncRoot(const fs::path& directory, const SyncRunOptions& options, SyncStats& stats,
                           std::string& error) const {
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) {
        error = "folder not found: " + directory.u8string() +
                " (if it is on a removable or network drive, is the drive connected?)";
        return false;
    }
    Database db;
    if (!openForWrite(location_, db, error, nullptr)) return false;
    return syncDirectory(directory, db, toSyncOptions(options, directory.u8string()), stats, error);
}

bool FluxService::syncAll(const SyncRunOptions& options, SyncAllResult& result, std::string& error) const {
    result = SyncAllResult{};
    Database db;
    if (!openForWrite(location_, db, error, nullptr)) return false;
    std::vector<RootInfo> roots;
    if (!listRoots(db, roots, error)) return false;
    if (roots.empty()) {
        error = "No folders are registered. Add a folder first.";
        return false;
    }
    for (const RootInfo& root : roots) {
        RootSyncResult entry;
        entry.rootPath = root.path;
        if (options.cancel != nullptr && options.cancel->load()) {
            entry.cancelled = true;  // not started
            result.roots.push_back(std::move(entry));
            continue;
        }
        if (!isAvailable(root.path)) {
            entry.unavailable = true;
            entry.error = "not available (drive disconnected or folder missing); its index entries were left as they are";
            result.roots.push_back(std::move(entry));
            continue;
        }
        entry.attempted = true;
        SyncOptions syncOptions = toSyncOptions(options, root.path);
        syncOptions.registerIfNoRoots = false;
        entry.ok = syncDirectory(fs::u8path(root.path), db, syncOptions, entry.stats, entry.error);
        entry.cancelled = !entry.ok && isCancelled(entry.error);
        result.roots.push_back(std::move(entry));
    }
    return true;
}

}  // namespace flux
