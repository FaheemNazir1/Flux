#include "flux/indexer.hpp"

#include "flux/database.hpp"
#include "flux/index_writer.hpp"
#include "flux/path_utils.hpp"
#include "flux/roots.hpp"
#include "flux/scanner.hpp"
#include "flux/tokenizer.hpp"
#include "flux/worker_pool.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <unordered_map>

namespace fs = std::filesystem;

namespace flux {

void IndexStats::merge(const IndexStats& other) {
    documents += other.documents;
    emptyDocuments += other.emptyDocuments;
    tokens += other.tokens;
    unsupportedFiles += other.unsupportedFiles;
    excludedDirectories += other.excludedDirectories;
    linkedDirectories += other.linkedDirectories;
    skipped.insert(skipped.end(), other.skipped.begin(), other.skipped.end());
}

FileStamp statFile(const fs::path& file) {
    FileStamp stamp;
    const std::uintmax_t size = fs::file_size(file, stamp.error);
    if (stamp.error) {
        return stamp;
    }
    const auto mtime = fs::last_write_time(file, stamp.error);
    if (stamp.error) {
        return stamp;
    }
    stamp.sizeBytes = size;
    stamp.mtimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count();
    stamp.ok = true;
    return stamp;
}

namespace {

// Reads a file whose size is already known (so no further stat call is needed).
ReadStatus readTextFileOfSize(const fs::path& path, std::uintmax_t size, std::string& content) {
    content.clear();
    if (size > kMaxFileBytes) {
        return ReadStatus::TooLarge;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return ReadStatus::Unreadable;
    }
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        content.clear();
        return ReadStatus::Unreadable;
    }
    if (content.find('\0') != std::string::npos) {
        content.clear();
        return ReadStatus::Binary;
    }
    return ReadStatus::Ok;
}

}  // namespace

ReadStatus readTextFile(const fs::path& path, std::string& content) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        content.clear();
        return ReadStatus::Unreadable;
    }
    return readTextFileOfSize(path, size, content);
}

ReadStatus readTextFileUnchangedSince(const fs::path& path, std::string& content,
                                      const FileStamp& before) {
    if (!before.ok) {
        content.clear();
        return ReadStatus::Unreadable;
    }
    const ReadStatus status = readTextFileOfSize(path, before.sizeBytes, content);
    if (status == ReadStatus::Unreadable) {
        return status;
    }
    // Any status (even Binary/TooLarge) is only trustworthy if the file did not move under us.
    std::error_code ec;
    const auto mtime = fs::last_write_time(path, ec);
    const std::int64_t mtimeNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count();
    if (ec || mtimeNs != before.mtimeNs || (status == ReadStatus::Ok && content.size() != before.sizeBytes)) {
        content.clear();
        return ReadStatus::Changed;
    }
    return status;
}

ReadStatus readStableTextFile(const fs::path& path, std::string& content, FileStamp& stamp) {
    stamp = statFile(path);
    return readTextFileUnchangedSince(path, content, stamp);
}

const char* describe(ReadStatus status) {
    switch (status) {
        case ReadStatus::Unreadable: return "unreadable";
        case ReadStatus::Binary: return "binary content";
        case ReadStatus::TooLarge: return "file too large";
        case ReadStatus::Changed: return "changed while being read";
        case ReadStatus::Ok: break;
    }
    return "ok";
}

ProcessedDocument buildProcessedDocument(const fs::path& file, const FileStamp& stamp,
                                         const std::vector<std::string>& tokens) {
    ProcessedDocument doc;
    doc.path = normalizedPath(file);
    doc.sizeBytes = stamp.sizeBytes;
    doc.mtimeNs = stamp.mtimeNs;
    doc.tokenCount = tokens.size();

    std::unordered_map<std::string, std::uint32_t> counts;
    for (const std::string& token : tokens) {
        ++counts[token];
    }
    doc.termFrequencies.assign(counts.begin(), counts.end());
    return doc;
}

ProcessedDocument buildProcessedDocument(const fs::path& file, const FileStamp& stamp,
                                         std::unordered_map<std::string, std::uint32_t>&& counts, std::size_t tokenCount) {
    ProcessedDocument doc;
    doc.path = normalizedPath(file);
    doc.sizeBytes = stamp.sizeBytes;
    doc.mtimeNs = stamp.mtimeNs;
    doc.tokenCount = tokenCount;
    doc.termFrequencies.reserve(counts.size());
    for (auto& entry : counts) doc.termFrequencies.emplace_back(entry.first, entry.second);
    return doc;
}

ProcessedDocument buildProcessedDocument(const fs::path& file, std::size_t sizeBytes,
                                         const std::vector<std::string>& tokens) {
    FileStamp stamp = statFile(file);
    stamp.sizeBytes = sizeBytes;
    return buildProcessedDocument(file, stamp, tokens);
}

namespace {

// Reads and tokenizes one file into `local`, which must be owned by the calling
// thread. When `wantDocument` is set, also returns the document for storage.
std::optional<ProcessedDocument> processFile(const fs::path& file, IndexStats& local,
                                             bool wantDocument) {
    try {
        std::string content;
        FileStamp stamp;
        // Nothing is stored without a database, so the extra stat calls that guard against a file
        // changing mid-read are only paid when a document is going to be persisted.
        const ReadStatus status = wantDocument ? readStableTextFile(file, content, stamp)
                                               : readTextFile(file, content);
        if (status != ReadStatus::Ok) {
            local.skipped.push_back(file.string() + ": " + describe(status));
            return std::nullopt;
        }
        const std::vector<std::string> tokens = tokenize(content);
        ++local.documents;
        local.tokens += tokens.size();
        if (tokens.empty()) {
            ++local.emptyDocuments;
        }
        if (!wantDocument) {
            return std::nullopt;
        }
        return buildProcessedDocument(file, stamp, tokens);
    } catch (const std::exception& e) {
        local.skipped.push_back(file.string() + ": error: " + e.what());
        return std::nullopt;
    }
}

}  // namespace

bool indexDirectory(const fs::path& root, IndexStats& stats, std::string& error, std::size_t threads,
                    Database* db) {
    IndexOptions options;
    options.threads = threads;
    options.db = db;
    return indexDirectory(root, stats, error, options);
}

bool indexDirectory(const fs::path& root, IndexStats& stats, std::string& error, const IndexOptions& options) {
    Database* const db = options.db;

    // Resolve the root first so every stored path has one spelling, however the user typed it.
    fs::path canonicalRoot;
    if (!canonicalDirectory(root, canonicalRoot, error)) {
        return false;
    }

    // A database with registered roots only accepts files from inside one of them, and the
    // documents then belong to that root.
    std::int64_t rootId = 0;
    if (db != nullptr && db->schemaVersion() >= 2) {
        std::vector<RootInfo> roots;
        if (!listRoots(*db, roots, error)) {
            return false;
        }
        const std::string key = rootKey(canonicalRoot);
        if (!roots.empty()) {
            const auto owner = std::find_if(roots.begin(), roots.end(),
                                            [&](const RootInfo& r) { return isPathUnder(key, r.path); });
            if (owner == roots.end()) {
                std::string list;
                for (const RootInfo& r : roots) list += (list.empty() ? "" : ", ") + r.path;
                error = "'" + key + "' is not inside any registered root (registered: " + list +
                        "). Register it first with: flux root add <directory>";
                return false;
            }
            rootId = owner->id;
        } else if (options.requireRegisteredRoot) {
            error = "no root is registered in this database. Register one first with: flux root add <directory>";
            return false;
        }
    }

    // Phase 1: discover files. Kept separate from processing.
    ScanResult scan;
    if (!scanDirectory(canonicalRoot, scan, error, options.scan)) {
        return false;
    }
    stats.unsupportedFiles += scan.unsupportedCount;
    stats.excludedDirectories += scan.excludedDirectories;
    stats.linkedDirectories += scan.linkedDirectories;
    stats.skipped.insert(stats.skipped.end(), scan.errors.begin(), scan.errors.end());
    if (options.progress) {
        options.progress("scanned", scan.files.size(), 0);
    }

    // Phase 2: process files in parallel. Each worker writes only to its own
    // IndexStats slot; slots are merged after the pool has joined. With a
    // database, workers also hand documents to the single writer thread.
    //
    // Declaration order matters for unwinding: the pool is destroyed (workers
    // joined) before the writer, so no worker can submit to a dead writer.
    const std::size_t workers = std::max<std::size_t>(options.threads, 1);
    std::vector<IndexStats> perWorker(workers);
    try {
        std::optional<IndexWriter> writer;
        if (db != nullptr) {
            writer.emplace(*db);
        }
        {
            WorkerPool pool(workers, workers * 4,
                            [&perWorker, &writer, rootId](std::size_t worker, const fs::path& file) {
                                auto doc = processFile(file, perWorker[worker], writer.has_value());
                                if (doc) {
                                    doc->rootId = rootId;
                                    writer->submit(std::move(*doc));
                                }
                            });
            std::size_t submitted = 0;
            for (const fs::path& file : scan.files) {
                if (!pool.submit(file)) {
                    break;
                }
                if (options.progress && ++submitted % kProgressInterval == 0) {
                    options.progress("processed", submitted, scan.files.size());
                }
            }
            pool.finish();
        }
        if (writer) {
            std::string writeError;
            if (!writer->finish(writeError)) {
                error = "database write failed: " + writeError;
                return false;
            }
        }
    } catch (const std::exception& e) {
        error = std::string("worker failure: ") + e.what();
        return false;
    }

    for (const IndexStats& local : perWorker) {
        stats.merge(local);
    }
    std::sort(stats.skipped.begin(), stats.skipped.end());
    return true;
}

}  // namespace flux
