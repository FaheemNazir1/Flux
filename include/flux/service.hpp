#pragma once

#include "flux/app_paths.hpp"
#include "flux/content_stats.hpp"
#include "flux/drives.hpp"
#include "flux/options.hpp"
#include "flux/roots.hpp"
#include "flux/search.hpp"
#include "flux/sync.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace flux {

// What a front end (the desktop app) needs from the engine, as plain synchronous calls that return
// data. Everything here reuses the existing engine functions (searchIndex, addRoot, removeRoot,
// syncDirectory, ...); there is no second index and no UI code. Each call opens its OWN database
// connection and closes it before returning, so calls from different threads never share a
// connection (SQLite WAL lets searches run while a sync is writing).
//
// The service holds only the database location. Calls are thread-safe with each other; it is up to
// the caller (the GUI's JobRunner) to run at most one writing operation at a time.

struct RootStatus {
    RootInfo info;
    bool available = false;  // is the folder reachable right now (a disconnected drive is not)
};

struct Dashboard {
    std::filesystem::path databasePath;
    DatabaseSource source = DatabaseSource::Default;
    bool databaseExists = false;      // false: nothing has been indexed yet (not an error)
    int schemaVersion = 0;
    std::int64_t documents = 0;
    std::int64_t terms = 0;
    std::int64_t unassignedDocuments = 0;
    std::uintmax_t sizeBytes = 0;
    std::vector<RootStatus> roots;

    // What the index holds, by how searchable the content is (see content_stats.hpp). Counted over all roots.
    bool contentKnown = false;
    ContentCounts content;
};

// A drive as Flux sees it: what Windows reports plus what the index knows about it.
struct DriveStatus {
    DriveInfo info;
    bool indexed = false;      // the whole drive is a registered root
    bool partlyIndexed = false;  // only some folders of it are registered roots
    std::vector<std::string> roots;  // registered roots on this drive ("D:/" or "D:/Projects")
    std::int64_t files = 0;          // documents stored for those roots
    std::int64_t lastSyncUnix = 0;   // newest successful sync of any of them; 0 = never
};

struct DriveView {
    std::vector<DriveStatus> drives;        // present now, by letter
    std::vector<RootStatus> notConnected;   // registered roots on a drive letter that is not present now
};

enum class ServiceError {
    None,
    NoIndex,     // the database does not exist yet
    EmptyIndex,  // it exists but holds no documents
    BadRequest,  // empty query, unknown root, bad folder
    Failed,      // anything else (database error, cancelled, ...)
};

struct SearchRequest {
    std::string query;
    std::size_t limit = kDefaultSearchLimit;
    std::string rootPath;  // empty: all registered roots; otherwise a registered root (any spelling)
    bool snippets = true;  // read each content hit's file to show where the words occur (see addSnippets)
};

struct SearchOutcome {
    bool ok = false;
    ServiceError kind = ServiceError::None;
    std::string error;
    std::vector<FileHit> hits;  // file name and content matches, best first (see searchFiles)
    double elapsedMs = 0;
};

struct SyncRunOptions {
    std::size_t threads = 0;  // 0: automatic
    bool includeSystemFolders = false;
    std::vector<std::string> excludedFolders;  // folders to skip, see ScanOptions::excludedFolders
    ContentRefresh contentRefresh = ContentRefresh::Needed;  // which already-indexed files are read again
    const std::atomic<bool>* pause = nullptr;  // while true, work waits (cancel still works)
    bool indexFileNames = true;  // also record the names of files of other types (found by name, never read)
    // (root, stage, done, total): stages are "scanning", "scanned", "processed", "committing". Called
    // from the thread that runs the operation.
    std::function<void(const std::string&, const char*, std::size_t, std::size_t)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

struct RootSyncResult {
    std::string rootPath;
    bool attempted = false;
    bool unavailable = false;  // skipped: the folder or drive is not reachable
    bool ok = false;
    bool cancelled = false;
    std::string error;
    SyncStats stats;
};

struct SyncAllResult {
    std::vector<RootSyncResult> roots;
};

class FluxService {
public:
    explicit FluxService(DatabaseLocation location) : location_(std::move(location)) {}

    const DatabaseLocation& location() const { return location_; }

    // Read-only. A missing database is reported as databaseExists = false, not as an error.
    bool loadDashboard(Dashboard& dashboard, std::string& error) const;

    // Read-only. The drives present now, with their index status. Never changes the index; a database
    // that does not exist yet is not an error (every drive is simply not indexed). Can take a few seconds
    // if a network drive does not answer, so call it from a background job.
    bool loadDrives(DriveView& view, std::string& error) const;

    // Read-only. Finds files by name and by content in every registered folder (or one), see searchFiles.
    SearchOutcome search(const SearchRequest& request) const;

    // Writing operations. Each creates the database (and the default data folder) if needed, and
    // upgrades an older database after making a backup; `notice` receives a sentence about that.
    bool addRoot(const std::filesystem::path& directory, AddRootResult& result, std::string& error,
                 std::string* notice = nullptr) const;
    bool removeRoot(const std::filesystem::path& directory, RemoveRootResult& result, std::string& error) const;

    // Synchronizes one registered root (the first root of an empty database is registered, as in the CLI).
    // On failure, including cancellation, the index is exactly as it was.
    bool syncRoot(const std::filesystem::path& directory, const SyncRunOptions& options, SyncStats& stats,
                  std::string& error) const;

    // Synchronizes every available registered root, one transaction per root. Roots that are not
    // available are skipped (and reported), never failed or emptied. Stops early on cancellation.
    bool syncAll(const SyncRunOptions& options, SyncAllResult& result, std::string& error) const;

private:
    DatabaseLocation location_;
};

}  // namespace flux
