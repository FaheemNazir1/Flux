#pragma once

#include "flux/database.hpp"
#include "flux/document.hpp"
#include "flux/work_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flux {

// The single thread that writes to the database. Producers submit work; the writer
// applies it in submission order inside batched transactions using prepared statements:
//   - submit(document)      store a document, replacing any record for the same path
//   - submitEnsureRoot(p)   make sure the root `p` is registered (adopting unassigned documents under
//                           it if it is new); later documents with rootId 0 belong to it, and later
//                           removals only ever delete documents of that root
//   - submitRemoval(path)   delete a document and (by cascade) its postings
//   - submitMetadata(k, v)  upsert a row of the `meta` table
//   - submitTermPrune()     delete terms that no posting references any more
//
// The writer owns the Database connection for its lifetime: do not use `db`
// from another thread until finish() has returned or the writer is destroyed.
//
// After a database error the writer rolls back the current transaction, remembers the
// first error, and keeps draining (discarding) submissions so producers never block.
class IndexWriter {
public:
    // Each transaction commits `batchSize` documents. Larger batches amortize the per-commit
    // cost (measured: see benchmarks/README.md); a failure rolls back at most one batch.
    static constexpr std::size_t kDefaultBatchSize = 1000;
    // Pass as batchSize to apply everything in ONE transaction: all or nothing.
    static constexpr std::size_t kSingleTransaction = std::numeric_limits<std::size_t>::max();

    explicit IndexWriter(Database& db, std::size_t queueCapacity = 64,
                         std::size_t batchSize = kDefaultBatchSize);
    ~IndexWriter();  // closes the queue and joins the thread; commits what was accepted unless aborted

    IndexWriter(const IndexWriter&) = delete;
    IndexWriter& operator=(const IndexWriter&) = delete;

    // Each returns false if the writer is already finishing; they block while the queue is full.
    bool submit(ProcessedDocument document);
    bool submitEnsureRoot(std::string rootPath);
    // Deletes the document at `path`, but only if it belongs to `rootId` (0: the current root from
    // submitEnsureRoot, or any root if none was set).
    bool submitRemoval(std::string path, std::int64_t rootId = 0);
    bool submitMetadata(std::string key, std::string value);
    bool submitTermPrune();

    // Discards everything not yet committed: the open transaction is rolled back and queued
    // work is dropped. finish() then reports failure. Use when the caller learns that the
    // batch of work it was feeding is invalid.
    void abort();

    // Stops accepting work, waits for the queue to drain and the final commit, and joins
    // the thread. Idempotent. Returns false and sets `error` if any operation failed
    // or the writer was aborted.
    bool finish(std::string& error);

    // Committed so far. documentsRemoved() counts submitRemoval calls that deleted a row.
    std::size_t documentsWritten() const { return written_; }
    std::size_t documentsRemoved() const { return removed_; }

private:
    struct Statements;

    struct Task {
        enum class Kind { Store, Remove, SetMeta, PruneTerms, EnsureRoot };
        Kind kind = Kind::Store;
        ProcessedDocument document;  // Store
        std::string key;             // Remove: path; SetMeta: key; EnsureRoot: root path
        std::string value;           // SetMeta
        std::int64_t rootId = 0;     // Remove
    };

    struct PendingPosting {
        std::int64_t termId;
        std::int64_t docId;
        std::uint32_t frequency;
    };

    // Postings are inserted in sorted chunks of at most this many, so one huge transaction
    // does not buffer every posting of the run in memory.
    static constexpr std::size_t kMaxPendingPostings = 500000;

    void run();
    bool prepareStatements(Statements& st);
    bool apply(Statements& st, const Task& task);
    bool writeDocument(Statements& st, const ProcessedDocument& doc);
    bool removeDocument(Statements& st, const std::string& path, std::int64_t rootId);
    bool ensureRoot(Statements& st, const std::string& rootPath);
    bool flushPostings(Statements& st);
    bool commitBatch(Statements& st);
    void fail(const std::string& message);
    void joinThread();

    Database& db_;
    BoundedQueue<Task> queue_;
    const std::size_t batchSize_;
    std::thread thread_;
    std::atomic<bool> aborted_{false};

    // Touched only by the writer thread until it is joined.
    bool inTransaction_ = false;
    std::int64_t currentRootId_ = 0;  // 0: none (see ProcessedDocument::rootId)
    std::size_t pendingInBatch_ = 0;
    std::size_t pendingRemovals_ = 0;
    // Postings of the open transaction, inserted sorted by (term, document) in chunks:
    // sequential B-tree inserts instead of random ones (see benchmarks/README.md).
    std::vector<PendingPosting> pendingPostings_;
    std::unordered_set<std::string> batchPaths_;  // paths whose postings are still pending
    bool failed_ = false;
    std::string error_;

    std::atomic<std::size_t> written_{0};
    std::atomic<std::size_t> removed_{0};
};

}  // namespace flux
