#include "flux/indexer.hpp"

#include "flux/scanner.hpp"
#include "flux/tokenizer.hpp"
#include "flux/worker_pool.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace flux {

void IndexStats::merge(const IndexStats& other) {
    documents += other.documents;
    emptyDocuments += other.emptyDocuments;
    tokens += other.tokens;
    unsupportedFiles += other.unsupportedFiles;
    skipped.insert(skipped.end(), other.skipped.begin(), other.skipped.end());
}

ReadStatus readTextFile(const fs::path& path, std::string& content) {
    content.clear();

    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        return ReadStatus::Unreadable;
    }
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

namespace {

const char* describe(ReadStatus status) {
    switch (status) {
        case ReadStatus::Unreadable: return "unreadable";
        case ReadStatus::Binary: return "binary content";
        case ReadStatus::TooLarge: return "file too large";
        case ReadStatus::Ok: break;
    }
    return "ok";
}

// Reads and tokenizes one file into `local`, which must be owned by the calling thread.
void processFile(const fs::path& file, IndexStats& local) {
    try {
        std::string content;
        const ReadStatus status = readTextFile(file, content);
        if (status != ReadStatus::Ok) {
            local.skipped.push_back(file.string() + ": " + describe(status));
            return;
        }
        const std::size_t count = tokenize(content).size();
        ++local.documents;
        local.tokens += count;
        if (count == 0) {
            ++local.emptyDocuments;
        }
    } catch (const std::exception& e) {
        local.skipped.push_back(file.string() + ": error: " + e.what());
    }
}

}  // namespace

bool indexDirectory(const fs::path& root, IndexStats& stats, std::string& error,
                    std::size_t threads) {
    // Phase 1: discover files. Kept separate from processing.
    ScanResult scan;
    if (!scanDirectory(root, scan, error)) {
        return false;
    }
    stats.unsupportedFiles += scan.unsupportedCount;
    stats.skipped.insert(stats.skipped.end(), scan.errors.begin(), scan.errors.end());

    // Phase 2: process files in parallel. Each worker writes only to its own
    // IndexStats slot; slots are merged after the pool has joined.
    const std::size_t workers = std::max<std::size_t>(threads, 1);
    std::vector<IndexStats> perWorker(workers);
    try {
        WorkerPool pool(workers, workers * 4, [&perWorker](std::size_t worker, const fs::path& file) {
            processFile(file, perWorker[worker]);
        });
        for (const fs::path& file : scan.files) {
            if (!pool.submit(file)) {
                break;
            }
        }
        pool.finish();
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
