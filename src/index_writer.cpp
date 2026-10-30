#include "flux/index_writer.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <unordered_map>

namespace flux {

struct IndexWriter::Statements {
    Statement deleteDocument;
    Statement insertDocument;
    Statement internTerm;
    Statement insertPosting;
    Statement setMeta;
    Statement pruneTerms;
    Statement removeFromRoot;
    Statement insertRoot;
    Statement selectRoot;
    Statement adoptDocuments;
    Statement insertStatus;
    std::unordered_map<std::string, std::int64_t> termIds;  // valid for committed and in-flight terms
};

IndexWriter::IndexWriter(Database& db, std::size_t queueCapacity, std::size_t batchSize)
    : db_(db), queue_(queueCapacity), batchSize_(std::max<std::size_t>(batchSize, 1)) {
    thread_ = std::thread(&IndexWriter::run, this);
}

IndexWriter::~IndexWriter() {
    joinThread();
}

bool IndexWriter::submit(ProcessedDocument document) {
    Task task;
    task.kind = Task::Kind::Store;
    task.document = std::move(document);
    return queue_.push(std::move(task));
}

bool IndexWriter::submitEnsureRoot(std::string rootPath) {
    Task task;
    task.kind = Task::Kind::EnsureRoot;
    task.key = std::move(rootPath);
    return queue_.push(std::move(task));
}

bool IndexWriter::submitRemoval(std::string path, std::int64_t rootId) {
    Task task;
    task.kind = Task::Kind::Remove;
    task.key = std::move(path);
    task.rootId = rootId;
    return queue_.push(std::move(task));
}

bool IndexWriter::submitMetadata(std::string key, std::string value) {
    Task task;
    task.kind = Task::Kind::SetMeta;
    task.key = std::move(key);
    task.value = std::move(value);
    return queue_.push(std::move(task));
}

bool IndexWriter::submitTermPrune() {
    Task task;
    task.kind = Task::Kind::PruneTerms;
    return queue_.push(std::move(task));
}

void IndexWriter::abort() {
    aborted_ = true;
}

bool IndexWriter::finish(std::string& error) {
    joinThread();  // join() makes the writer thread's state visible here
    if (failed_) {
        error = error_;
        return false;
    }
    return true;
}

void IndexWriter::joinThread() {
    queue_.close();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void IndexWriter::fail(const std::string& message) {
    if (inTransaction_) {
        db_.rollback();
        inTransaction_ = false;
        pendingInBatch_ = 0;
        pendingRemovals_ = 0;
    }
    pendingPostings_.clear();
    batchPaths_.clear();
    if (!failed_) {
        failed_ = true;
        error_ = message;
    }
}

bool IndexWriter::prepareStatements(Statements& st) {
    return st.deleteDocument.prepare(db_, "DELETE FROM documents WHERE path = ?") &&
           st.insertDocument.prepare(
               db_,
               "INSERT INTO documents(path, size_bytes, mtime_ns, token_count, indexed_at, root_id) "
               "VALUES (?, ?, ?, ?, ?, ?)") &&
           st.removeFromRoot.prepare(db_, "DELETE FROM documents WHERE path = ?1 AND root_id IS ?2") &&
           st.insertRoot.prepare(db_, "INSERT INTO roots(path, added_at) VALUES (?, ?) "
                                      "ON CONFLICT(path) DO NOTHING") &&
           st.selectRoot.prepare(db_, "SELECT id FROM roots WHERE path = ?") &&
           (!db_.hasContentStatus() ||
            st.insertStatus.prepare(db_, "INSERT OR REPLACE INTO content_status(doc_id, status, extractor, detail) "
                                         "VALUES (?, ?, ?, ?)")) &&
           st.adoptDocuments.prepare(db_, "UPDATE documents SET root_id = ?1 WHERE root_id IS NULL AND "
                                          "substr(path, 1, length(?2)) = ?2") &&
           st.internTerm.prepare(
               db_,
               "INSERT INTO terms(term) VALUES (?) "
               "ON CONFLICT(term) DO UPDATE SET term = excluded.term RETURNING id") &&
           st.insertPosting.prepare(
               db_, "INSERT INTO postings(term_id, doc_id, frequency) VALUES (?, ?, ?)") &&
           st.setMeta.prepare(
               db_, "INSERT INTO meta(key, value) VALUES (?, ?) "
                    "ON CONFLICT(key) DO UPDATE SET value = excluded.value") &&
           st.pruneTerms.prepare(
               db_, "DELETE FROM terms WHERE NOT EXISTS "
                    "(SELECT 1 FROM postings p WHERE p.term_id = terms.id)");
}

bool IndexWriter::writeDocument(Statements& st, const ProcessedDocument& doc) {
    // A path stored earlier whose postings are still pending would be left pointing at a deleted
    // row by the delete below. Insert them first so the delete cascades over them.
    if (batchPaths_.count(doc.path) > 0 && !flushPostings(st)) {
        return false;
    }
    batchPaths_.insert(doc.path);

    // Replace any previous record for this path; postings go with it (ON DELETE CASCADE).
    if (!st.deleteDocument.bindText(1, doc.path) ||
        st.deleteDocument.step() != Statement::Step::Done || !st.deleteDocument.reset()) {
        return false;
    }

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const std::int64_t indexedAt = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    if (!st.insertDocument.bindText(1, doc.path) ||
        !st.insertDocument.bindInt64(2, static_cast<std::int64_t>(doc.sizeBytes)) ||
        !st.insertDocument.bindInt64(3, doc.mtimeNs) ||
        !st.insertDocument.bindInt64(4, static_cast<std::int64_t>(doc.tokenCount)) ||
        !st.insertDocument.bindInt64(5, indexedAt)) {
        return false;
    }
    const std::int64_t rootId = doc.rootId != 0 ? doc.rootId : currentRootId_;
    if (!(rootId != 0 ? st.insertDocument.bindInt64(6, rootId) : st.insertDocument.bindNull(6)) ||
        st.insertDocument.step() != Statement::Step::Done || !st.insertDocument.reset()) {
        return false;
    }
    const std::int64_t docId = db_.lastInsertRowId();
    if (doc.contentStatus != 0 && db_.hasContentStatus()) {
        if (!st.insertStatus.bindInt64(1, docId) || !st.insertStatus.bindInt64(2, doc.contentStatus) ||
            !st.insertStatus.bindInt64(3, doc.extractorVersion) || !st.insertStatus.bindText(4, doc.contentDetail) ||
            st.insertStatus.step() != Statement::Step::Done || !st.insertStatus.reset()) {
            return false;
        }
    }

    for (const auto& [term, frequency] : doc.termFrequencies) {
        std::int64_t termId;
        const auto cached = st.termIds.find(term);
        if (cached != st.termIds.end()) {
            termId = cached->second;
        } else {
            if (!st.internTerm.bindText(1, term) || st.internTerm.step() != Statement::Step::Row) {
                return false;
            }
            termId = st.internTerm.columnInt64(0);
            if (!st.internTerm.reset()) {
                return false;
            }
            st.termIds.emplace(term, termId);
        }
        pendingPostings_.push_back({termId, docId, frequency});
    }
    return pendingPostings_.size() < kMaxPendingPostings || flushPostings(st);
}

bool IndexWriter::removeDocument(Statements& st, const std::string& path, std::int64_t rootId) {
    if (batchPaths_.count(path) > 0 && !flushPostings(st)) {
        return false;
    }
    // Given a root, only that root's documents can be deleted: a wrong path can never remove
    // another root's data.
    const std::int64_t scope = rootId != 0 ? rootId : currentRootId_;
    Statement& remove = scope != 0 ? st.removeFromRoot : st.deleteDocument;
    if (!remove.bindText(1, path) || (scope != 0 && !remove.bindInt64(2, scope)) ||
        remove.step() != Statement::Step::Done) {
        return false;
    }
    if (db_.changes() > 0) {
        ++pendingRemovals_;
    }
    return remove.reset();
}

bool IndexWriter::ensureRoot(Statements& st, const std::string& rootPath) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    if (!st.insertRoot.bindText(1, rootPath) ||
        !st.insertRoot.bindInt64(2, std::chrono::duration_cast<std::chrono::seconds>(now).count()) ||
        st.insertRoot.step() != Statement::Step::Done) {
        return false;
    }
    const bool created = db_.changes() > 0;
    if (!st.insertRoot.reset()) return false;

    if (!st.selectRoot.bindText(1, rootPath) || st.selectRoot.step() != Statement::Step::Row) {
        return false;
    }
    currentRootId_ = st.selectRoot.columnInt64(0);
    if (!st.selectRoot.reset()) return false;

    if (created) {
        // A newly registered root adopts documents that were indexed before roots existed.
        std::string prefix = rootPath;
        if (prefix.empty() || prefix.back() != '/') prefix.push_back('/');
        if (!st.adoptDocuments.bindInt64(1, currentRootId_) || !st.adoptDocuments.bindText(2, prefix) ||
            st.adoptDocuments.step() != Statement::Step::Done || !st.adoptDocuments.reset()) {
            return false;
        }
    }
    return true;
}

bool IndexWriter::apply(Statements& st, const Task& task) {
    switch (task.kind) {
        case Task::Kind::Store:
            if (!writeDocument(st, task.document)) return false;
            ++pendingInBatch_;
            return true;
        case Task::Kind::Remove:
            return removeDocument(st, task.key, task.rootId);
        case Task::Kind::EnsureRoot:
            return ensureRoot(st, task.key);
        case Task::Kind::SetMeta:
            return st.setMeta.bindText(1, task.key) && st.setMeta.bindText(2, task.value) &&
                   st.setMeta.step() == Statement::Step::Done && st.setMeta.reset();
        case Task::Kind::PruneTerms:
            if (!flushPostings(st) || st.pruneTerms.step() != Statement::Step::Done ||
                !st.pruneTerms.reset()) {
                return false;
            }
            st.termIds.clear();  // ids of deleted terms may be reused by later inserts
            return true;
    }
    return false;
}

bool IndexWriter::flushPostings(Statements& st) {
    std::sort(pendingPostings_.begin(), pendingPostings_.end(),
              [](const PendingPosting& a, const PendingPosting& b) {
                  return a.termId != b.termId ? a.termId < b.termId : a.docId < b.docId;
              });
    for (const PendingPosting& p : pendingPostings_) {
        if (!st.insertPosting.bindInt64(1, p.termId) || !st.insertPosting.bindInt64(2, p.docId) ||
            !st.insertPosting.bindInt64(3, p.frequency) ||
            st.insertPosting.step() != Statement::Step::Done || !st.insertPosting.reset()) {
            return false;
        }
    }
    pendingPostings_.clear();
    batchPaths_.clear();
    return true;
}

bool IndexWriter::commitBatch(Statements& st) {
    if (!inTransaction_) {
        return true;
    }
    if (!flushPostings(st) || !db_.commit()) {
        return false;
    }
    inTransaction_ = false;
    written_ += pendingInBatch_;
    removed_ += pendingRemovals_;
    pendingInBatch_ = 0;
    pendingRemovals_ = 0;
    return true;
}

void IndexWriter::run() {
    try {
        Statements st;
        if (!prepareStatements(st)) {
            fail("cannot prepare statements: " + db_.lastError());
        }

        while (auto task = queue_.pop()) {
            if (failed_) {
                continue;  // keep draining so producers are never blocked
            }
            if (aborted_) {
                fail("aborted: nothing was committed from the open transaction");
                continue;
            }
            if (!inTransaction_) {
                if (!db_.begin()) {
                    fail("cannot begin transaction: " + db_.lastError());
                    continue;
                }
                inTransaction_ = true;
            }
            if (!apply(st, *task)) {
                fail("cannot apply " + (task->kind == Task::Kind::Store ? task->document.path : task->key) +
                     ": " + db_.lastError());
                continue;
            }
            if (pendingInBatch_ >= batchSize_ && !commitBatch(st)) {
                fail("cannot commit batch: " + db_.lastError());
            }
        }
        if (!failed_ && aborted_) {
            fail("aborted: nothing was committed from the open transaction");
        }
        if (!failed_ && !commitBatch(st)) {
            fail("cannot commit batch: " + db_.lastError());
        }
    } catch (const std::exception& e) {
        fail(std::string("writer error: ") + e.what());
        while (queue_.pop()) {  // never leave producers blocked on a full queue
        }
    }
}

}  // namespace flux
