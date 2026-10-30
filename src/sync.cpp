#include "flux/sync.hpp"

#include "flux/index_writer.hpp"
#include "flux/indexer.hpp"
#include "flux/path_utils.hpp"
#include "flux/roots.hpp"
#include "flux/extract.hpp"
#include "flux/tokenizer.hpp"
#include "flux/worker_pool.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace flux {
namespace {

struct StoredRecord {
    std::int64_t sizeBytes = 0;
    std::int64_t mtimeNs = 0;
    std::int64_t indexedAt = 0;  // unix seconds when the record was written
    std::int64_t tokenCount = 0;
    int status = 0;     // ExtractStatus recorded for the file; 0 = none recorded
    int extractor = 0;  // version of the reader that produced it
};
using RecordMap = std::unordered_map<std::string, StoredRecord>;

// True if the record's metadata can be relied on: the file was last modified clearly before
// the record was written, so a later modification would have changed the timestamp.
bool isTrustworthy(const StoredRecord& record) {
    const double mtimeUnixSeconds =
        (static_cast<double>(record.mtimeNs) - static_cast<double>(fileClockOffsetNs())) / 1e9;
    return mtimeUnixSeconds + kRacyWindowSeconds < static_cast<double>(record.indexedAt);
}

struct WorkerResult {
    std::size_t added = 0, updated = 0, refreshed = 0, unchanged = 0, kept = 0, tokens = 0, nameOnly = 0;
    std::size_t byStatus[16] = {};  // files read in this sync, by ExtractStatus
    std::vector<std::string> removals;  // records to delete: the file is gone or no longer indexable
    std::vector<std::string> warnings;
};

// True if a file with a reader must be read although its size and date are unchanged: its content was
// never extracted (a name-only record from before readers existed), was extracted by an older reader, or
// the caller asked for a refresh.
bool contentNeedsExtraction(const fs::path& file, ContentKind kind, const StoredRecord& record, ContentRefresh refresh) {
    if (kind == ContentKind::None) return false;
    if (refresh == ContentRefresh::All) return true;
    if (record.status == 0) {
        // No status recorded: valid if it is a plain-text type that was indexed with words (the old behaviour).
        return !(record.tokenCount > 0 && isLegacyTextFile(file));
    }
    const ExtractStatus status = static_cast<ExtractStatus>(record.status);
    if (status == ExtractStatus::Unreadable) return true;  // transient: tried again at every sync
    if (record.extractor < extractorVersion(kind)) return true;
    if (refresh == ContentRefresh::RetryFailed) {
        return !statusHasText(status) && status != ExtractStatus::Empty && status != ExtractStatus::ImageOnly;
    }
    return false;
}

void countStatus(WorkerResult& out, ExtractStatus status) {
    const int index = static_cast<int>(status);
    if (index >= 0 && index < 16) ++out.byStatus[index];
}

// Examines one eligible file and, if it needs (re)indexing, hands it to the writer.
// `existing` is only read, so concurrent calls are safe.
//
// With `namesToo`, a file whose content cannot be indexed (no reader, encrypted, damaged, binary, over the
// size limit) is stored by NAME only: a record with no words, found by filename search. Why it has no
// content is recorded (content_status) so the app can say so.
//
// A file that cannot be read NOW (access denied, in use) never costs an existing record its content:
// the old record is kept and the file is tried again at the next sync.
void examineFile(const fs::path& file, const RecordMap& existing, IndexWriter& writer, std::int64_t rootId,
                 bool namesToo, const SyncOptions& options, WorkerResult& out) {
    const std::string key = normalizedPath(file);
    try {
        const auto record = existing.find(key);
        const bool known = record != existing.end();

        const FileStamp stamp = statFile(file);
        if (!stamp.ok) {
            if (stamp.error == std::errc::no_such_file_or_directory) {
                // Listed by the scan but gone by now: it was deleted.
                if (known) out.removals.push_back(key);
                out.warnings.push_back(key + ": disappeared during sync");
            } else {
                ++out.kept;
                out.warnings.push_back(key + ": cannot check, record kept (" + stamp.error.message() + ")");
            }
            return;
        }

        const ContentKind kind = contentKindFor(file);
        const bool sameMetadata = known &&
                                  static_cast<std::uint64_t>(record->second.sizeBytes) == stamp.sizeBytes &&
                                  record->second.mtimeNs == stamp.mtimeNs;
        const bool nameOnlyType = namesToo && kind == ContentKind::None;
        const bool needsContent = known && contentNeedsExtraction(file, kind, record->second, options.contentRefresh);
        if (sameMetadata && !needsContent && (nameOnlyType || isTrustworthy(record->second))) {
            ++out.unchanged;
            if (nameOnlyType) ++out.nameOnly;
            return;
        }

        const auto submitNameOnly = [&](ExtractStatus status, const std::string& detail) {
            ProcessedDocument document = buildProcessedDocument(file, stamp, std::vector<std::string>{});
            document.rootId = rootId;
            if (kind != ContentKind::None) {
                document.contentStatus = static_cast<int>(status);
                document.extractorVersion = extractorVersion(kind);
                document.contentDetail = detail;
            }
            writer.submit(std::move(document));
            ++out.nameOnly;
            if (!known) ++out.added;
            else ++out.updated;
        };
        if (nameOnlyType) {  // no reader: only its name, size and date are recorded
            submitNameOnly(ExtractStatus::Unsupported, {});
            return;
        }

        ExtractLimits limits = options.extractLimits;
        limits.cancel = options.cancel;
        ExtractResult result = extractContent(file, limits);

        // A file that changed while it was being read is neither trusted nor stored; the next sync sees it again.
        {
            const FileStamp after = statFile(file);
            if (!after.ok || after.mtimeNs != stamp.mtimeNs || after.sizeBytes != stamp.sizeBytes) {
                if (result.status != ExtractStatus::Unreadable) {
                    ++out.kept;
                    out.warnings.push_back(key + ": changed while being read, record kept");
                    return;
                }
            }
        }
        countStatus(out, result.status);

        switch (result.status) {
            case ExtractStatus::Ok:
            case ExtractStatus::Truncated: {
                std::unordered_map<std::string, std::uint32_t> counts;
                const std::size_t tokenCount = tokenizeCounts(result.text, counts, kind == ContentKind::PlainText ? 0 : 48);
                result.text.clear();
                result.text.shrink_to_fit();
                out.tokens += tokenCount;
                ProcessedDocument document = buildProcessedDocument(file, stamp, std::move(counts), tokenCount);
                document.rootId = rootId;  // 0 while the root is being registered: the writer fills it in
                document.contentStatus = static_cast<int>(result.status);
                document.extractorVersion = extractorVersion(kind);
                document.contentDetail = result.detail;
                writer.submit(std::move(document));
                if (!known) ++out.added;
                else if (sameMetadata && !needsContent) ++out.refreshed;
                else ++out.updated;
                return;
            }
            case ExtractStatus::Unreadable:
                if (known) {  // keep what is already searchable; retry next time
                    ++out.kept;
                    out.warnings.push_back(key + ": cannot be read now (" + result.detail + "), previous content kept");
                    return;
                }
                if (namesToo) {
                    out.warnings.push_back(key + ": cannot be read now (" + result.detail + "), indexed by name");
                    submitNameOnly(result.status, result.detail);
                } else {
                    ++out.kept;
                    out.warnings.push_back(key + ": cannot be read now (" + result.detail + ")");
                }
                return;
            case ExtractStatus::Empty:      // readable files that simply hold no (searchable) text are
            case ExtractStatus::ImageOnly:  // still documents, as an empty .txt always was
                submitNameOnly(result.status, result.detail);
                return;
            case ExtractStatus::Encrypted:
            case ExtractStatus::Corrupt:
            case ExtractStatus::Unsupported:
            case ExtractStatus::TooLarge:
            case ExtractStatus::Binary:
                if (namesToo) {  // its content is not searchable, but it can still be found by name
                    submitNameOnly(result.status, result.detail);
                    return;
                }
                // Positively not indexable now: its old content must not stay searchable.
                if (known) out.removals.push_back(key);
                out.warnings.push_back(key + ": " + extractStatusName(result.status) +
                                       (result.detail.empty() ? "" : " (" + result.detail + ")") + ", not indexed");
                return;
        }
    } catch (const std::exception& e) {
        ++out.kept;
        out.warnings.push_back(key + ": error: " + e.what() + ", record kept");
    }
}

// The stored records of one root. With rootId 0 (a root that is about to be registered) these are
// the unassigned documents under `rootPath`, which the registration will adopt.
bool loadRecords(Database& db, std::int64_t rootId, const std::string& rootPath, RecordMap& records,
                 std::string& error) {
    Statement statement;
    const std::string columns = db.hasContentStatus()
        ? "SELECT d.path, d.size_bytes, d.mtime_ns, d.indexed_at, d.token_count, COALESCE(c.status, 0), COALESCE(c.extractor, 0) "
          "FROM documents d LEFT JOIN content_status c ON c.doc_id = d.id "
        : "SELECT d.path, d.size_bytes, d.mtime_ns, d.indexed_at, d.token_count, 0, 0 FROM documents d ";
    const bool prepared =
        rootId != 0
            ? statement.prepare(db, columns + "WHERE d.root_id = ?1") && statement.bindInt64(1, rootId)
            : statement.prepare(db, columns + "WHERE d.root_id IS NULL AND substr(d.path, 1, length(?1)) = ?1") &&
                  statement.bindText(1, rootPrefix(rootPath));
    if (!prepared) {
        error = "cannot read documents: " + db.lastError();
        return false;
    }
    Statement::Step step;
    while ((step = statement.step()) == Statement::Step::Row) {
        records[statement.columnText(0)] = {statement.columnInt64(1), statement.columnInt64(2), statement.columnInt64(3),
                                            statement.columnInt64(4), static_cast<int>(statement.columnInt64(5)),
                                            static_cast<int>(statement.columnInt64(6))};
    }
    if (step == Statement::Step::Error) {
        error = "cannot read documents: " + db.lastError();
        return false;
    }
    return true;
}

}  // namespace

bool syncDirectory(const fs::path& root, Database& db, const SyncOptions& options, SyncStats& stats,
                   std::string& error) {
    stats = SyncStats{};

    // 1. Identify the root. A root that is not available (disconnected drive, deleted folder) fails
    //    here, before anything is read from or written to the database.
    fs::path canonicalRoot;
    if (!canonicalDirectory(root, canonicalRoot, error)) {
        return false;
    }
    const std::string rootStr = rootKey(canonicalRoot);
    stats.rootPath = rootStr;

    // 2. The root must be registered. The first root of a database may be registered by its first
    //    sync (Milestone 5 behaviour); any further root has to be added explicitly.
    std::vector<RootInfo> roots;
    if (!listRoots(db, roots, error)) {
        return false;
    }
    std::int64_t rootId = 0;
    for (const RootInfo& registered : roots) {
        if (registered.path == rootStr) rootId = registered.id;
    }
    if (rootId == 0) {
        if (!roots.empty() || !options.registerIfNoRoots) {
            const auto container = std::find_if(roots.begin(), roots.end(), [&](const RootInfo& r) {
                return isPathUnder(rootStr, r.path);
            });
            std::string list;
            for (const RootInfo& r : roots) list += (list.empty() ? "" : ", ") + r.path;
            error = "'" + rootStr + "' is not a registered root";
            if (container != roots.end()) {
                error += "; it is inside the registered root '" + container->path + "' (sync that root instead)";
            } else {
                error += " (registered: " + (list.empty() ? std::string("none") : list) +
                         "). Register it first with: flux root add <directory>";
            }
            return false;
        }
        stats.registeredRoot = true;
    }

    RecordMap existing;
    if (!loadRecords(db, rootId, rootStr, existing, error)) {
        return false;
    }

    // 3. Discover what is on disk. A failure to list the root itself aborts before any change.
    const auto cancelled = [&options] { return options.cancel != nullptr && options.cancel->load(); };
    if (options.progress) {
        options.progress("scanning", 0, 0);
    }
    ScanResult scan;
    ScanOptions scanOptions = options.scanOptions;
    scanOptions.cancel = options.cancel;
    const bool scanned = options.scan ? options.scan(canonicalRoot, scan, error)
                                      : scanDirectory(canonicalRoot, scan, error, scanOptions);
    if (!scanned) {
        if (cancelled()) error = "cancelled: no changes were applied to the index";
        return false;
    }
    stats.eligibleFiles = scan.files.size();
    stats.unsupportedFiles = scan.unsupportedCount;
    stats.excludedDirectories = scan.excludedDirectories;
    stats.linkedDirectories = scan.linkedDirectories;
    stats.warnings = scan.errors;
    stats.skippedDirectories = scan.skippedDirectories;
    stats.accessDeniedDirectories = scan.accessDeniedDirectories;
    stats.accessDeniedPaths = scan.accessDeniedPaths;
    if (options.progress) {
        options.progress("scanned", scan.files.size(), 0);
    }

    std::unordered_set<std::string> present;
    present.reserve(scan.files.size());
    for (const fs::path& file : scan.files) {
        present.insert(normalizedPath(file));
    }
    std::vector<std::string> unverifiable;
    for (const fs::path& path : scan.unverifiable) {
        unverifiable.push_back(normalizedPath(path));
    }

    // 4. Process in parallel; unchanged files are skipped after a stat. Everything goes into
    //    one transaction, so an abort below leaves the database untouched.
    const std::size_t workers = std::max<std::size_t>(options.threads, 1);
    std::vector<WorkerResult> perWorker(workers);
    IndexWriter writer(db, 64, IndexWriter::kSingleTransaction);
    std::set<std::string> removals;  // sorted: deterministic order
    bool aborted = false;
    try {
        if (rootId == 0) {
            // Registered inside the same transaction as everything else (and adopting the documents
            // indexed before roots existed), so a failed sync leaves no half-registered root behind.
            writer.submitEnsureRoot(rootStr);
        }
        {
            WorkerPool pool(workers, workers * 4,
                            [&](std::size_t worker, const fs::path& file) {
                                waitWhilePaused(options.scanOptions.pause, options.cancel);
                                if (cancelled()) return;  // drain quickly; the result is discarded below
                                examineFile(file, existing, writer, rootId, options.scanOptions.includeFileNames, options, perWorker[worker]);
                            });
            std::size_t submitted = 0;
            for (const fs::path& file : scan.files) {
                if (cancelled() || !pool.submit(file)) break;
                if (options.progress && ++submitted % kProgressInterval == 0) {
                    options.progress("processed", submitted, scan.files.size());
                }
            }
            pool.finish();  // rethrows a worker exception
        }
        if (cancelled()) {
            error = "cancelled: no changes were applied to the index";
            aborted = true;
        }

        for (WorkerResult& result : perWorker) {
            if (aborted) break;
            stats.added += result.added;
            stats.updated += result.updated;
            stats.refreshed += result.refreshed;
            stats.unchanged += result.unchanged;
            stats.keptUnverified += result.kept;
            stats.tokens += result.tokens;
            stats.nameOnlyFiles += result.nameOnly;
            stats.extractedFiles += result.byStatus[static_cast<int>(ExtractStatus::Ok)] +
                                    result.byStatus[static_cast<int>(ExtractStatus::Truncated)];
            stats.passwordProtectedFiles += result.byStatus[static_cast<int>(ExtractStatus::Encrypted)];
            stats.accessDeniedFiles += result.byStatus[static_cast<int>(ExtractStatus::Unreadable)];
            stats.unsupportedFormatFiles += result.byStatus[static_cast<int>(ExtractStatus::Unsupported)];
            stats.corruptedFiles += result.byStatus[static_cast<int>(ExtractStatus::Corrupt)];
            stats.imageOnlyFiles += result.byStatus[static_cast<int>(ExtractStatus::ImageOnly)];
            stats.noTextFiles += result.byStatus[static_cast<int>(ExtractStatus::Empty)];
            stats.tooLargeFiles += result.byStatus[static_cast<int>(ExtractStatus::TooLarge)];
            stats.binaryFiles += result.byStatus[static_cast<int>(ExtractStatus::Binary)];
            removals.insert(result.removals.begin(), result.removals.end());
            stats.warnings.insert(stats.warnings.end(), result.warnings.begin(), result.warnings.end());
        }

        // 5. Only now, with every file examined, decide what is stale: a record under the root
        //    whose file was not found, unless it lies where the scan could not look.
        for (const auto& entry : existing) {
            if (aborted) break;
            const std::string& path = entry.first;
            if (!isPathUnder(path, rootStr) || present.count(path) > 0) continue;
            const bool unknown = std::any_of(unverifiable.begin(), unverifiable.end(),
                                             [&](const std::string& dir) { return isPathUnder(path, dir); });
            if (unknown) {
                ++stats.keptUnverified;
            } else {
                removals.insert(path);
            }
        }

        if (!aborted) {
            // Recorded in the same transaction: a root shows a "last successful sync" only if it was one.
            writer.submitMetadata(kLastSyncKeyPrefix + rootStr,
                                  std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                     std::chrono::system_clock::now().time_since_epoch()).count()));
            for (const std::string& path : removals) {
                writer.submitRemoval(path, rootId);  // scoped to this root: cannot delete another root's document
            }
            const bool changed = stats.added + stats.updated + stats.refreshed + removals.size() > 0;
            if (changed) {
                writer.submitTermPrune();
            }
        }
    } catch (const std::exception& e) {
        error = std::string("worker failure: ") + e.what();
        aborted = true;
    }

    if (aborted) {
        writer.abort();
        std::string ignored;
        writer.finish(ignored);
        return false;
    }

    if (options.progress) {
        options.progress("committing", 0, 0);
    }
    if (cancelled()) {  // last chance before the commit: nothing has been committed yet
        writer.abort();
        std::string ignored;
        writer.finish(ignored);
        error = "cancelled: no changes were applied to the index";
        return false;
    }
    std::string writeError;
    if (!writer.finish(writeError)) {
        error = "database write failed (no changes were applied): " + writeError;
        return false;
    }
    stats.removed = writer.documentsRemoved();
    std::sort(stats.warnings.begin(), stats.warnings.end());
    return true;
}

}  // namespace flux
