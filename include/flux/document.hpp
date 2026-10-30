#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace flux {

// One tokenized document, ready to be stored. Produced by worker threads.
struct ProcessedDocument {
    // Root the document belongs to. 0 means "the writer's current root" (set by submitEnsureRoot),
    // or no root at all if none has been set (documents indexed by the legacy plain command).
    std::int64_t rootId = 0;
    std::string path;  // absolute, normalized, UTF-8
    std::uint64_t sizeBytes = 0;
    std::int64_t mtimeNs = 0;  // last write time in the file clock's units (compared, never displayed)
    std::size_t tokenCount = 0;
    std::vector<std::pair<std::string, std::uint32_t>> termFrequencies;  // distinct term -> occurrences

    // Why the file has or lacks searchable content (an ExtractStatus value, see extract.hpp), recorded in
    // `content_status` when the database has that table. 0 = not recorded (legacy callers).
    int contentStatus = 0;
    int extractorVersion = 0;
    std::string contentDetail;
};

}  // namespace flux
