#include "flux/content_stats.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace flux {
namespace {

std::string extensionOf(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return {};
    std::string ext = name.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

void add(ContentCounts& counts, ContentCategory category) {
    ++counts.filenameIndexed;
    switch (category) {
        case ContentCategory::Extracted: ++counts.extracted; break;
        case ContentCategory::Awaiting: ++counts.awaiting; break;
        case ContentCategory::Unsupported: ++counts.unsupported; break;
        case ContentCategory::PasswordProtected: ++counts.passwordProtected; break;
        case ContentCategory::AccessDenied: ++counts.accessDenied; break;
        case ContentCategory::Corrupted: ++counts.corrupted; break;
        case ContentCategory::NoText: ++counts.noText; break;
        case ContentCategory::ImageOnly: ++counts.imageOnly; break;
        case ContentCategory::TooLarge: ++counts.tooLarge; break;
        case ContentCategory::Binary: ++counts.binary; break;
    }
}

std::string rowsQuery(const Database& db, bool byRoot) {
    const std::string select = db.hasContentStatus()
        ? "SELECT d.path, d.token_count, COALESCE(c.status, 0), COALESCE(c.extractor, 0), COALESCE(c.detail, '') "
          "FROM documents d LEFT JOIN content_status c ON c.doc_id = d.id "
        : "SELECT d.path, d.token_count, 0, 0, '' FROM documents d ";
    return select + (byRoot ? "WHERE d.root_id = ?1" : "");
}

}  // namespace

const char* contentCategoryName(ContentCategory category) {
    switch (category) {
        case ContentCategory::Extracted: return "extracted";
        case ContentCategory::Awaiting: return "awaiting extraction";
        case ContentCategory::Unsupported: return "unsupported format";
        case ContentCategory::PasswordProtected: return "password required";
        case ContentCategory::AccessDenied: return "access denied";
        case ContentCategory::Corrupted: return "corrupted";
        case ContentCategory::NoText: return "no text";
        case ContentCategory::ImageOnly: return "image only (needs OCR)";
        case ContentCategory::TooLarge: return "too large";
        case ContentCategory::Binary: return "binary";
    }
    return "unknown";
}

ContentCategory categorizeContent(const std::string& path, std::int64_t tokenCount, int status, int extractor) {
    (void)extractor;
    const ContentKind kind = contentKindFor(fs::u8path(path));
    if (status == 0) {
        if (kind == ContentKind::None) return ContentCategory::Unsupported;
        // Indexed with words before statuses existed: valid for the plain-text types of that time.
        if (tokenCount > 0 && isLegacyTextFile(fs::u8path(path))) return ContentCategory::Extracted;
        return ContentCategory::Awaiting;
    }
    switch (static_cast<ExtractStatus>(status)) {
        case ExtractStatus::Ok:
        case ExtractStatus::Truncated: return ContentCategory::Extracted;
        case ExtractStatus::Empty: return ContentCategory::NoText;
        case ExtractStatus::ImageOnly: return ContentCategory::ImageOnly;
        case ExtractStatus::Encrypted: return ContentCategory::PasswordProtected;
        case ExtractStatus::Corrupt: return ContentCategory::Corrupted;
        case ExtractStatus::Unsupported: return ContentCategory::Unsupported;
        case ExtractStatus::TooLarge: return ContentCategory::TooLarge;
        case ExtractStatus::Unreadable: return ContentCategory::AccessDenied;
        case ExtractStatus::Binary: return ContentCategory::Binary;
    }
    return ContentCategory::Awaiting;
}

std::string explainContent(const std::string& path, ContentCategory category, const std::string& detail) {
    std::string reason;
    switch (category) {
        case ContentCategory::Extracted: reason = extractStatusDescription(ExtractStatus::Ok); break;
        case ContentCategory::Awaiting: reason = "Its text has not been read yet. It will be at the next sync (or run: flux reindex --content)."; break;
        case ContentCategory::Unsupported: reason = "Flux cannot read inside this kind of file (" + unsupportedReason(fs::u8path(path)) + "); it is searchable by name."; break;
        case ContentCategory::PasswordProtected: reason = extractStatusDescription(ExtractStatus::Encrypted); break;
        case ContentCategory::AccessDenied: reason = extractStatusDescription(ExtractStatus::Unreadable); break;
        case ContentCategory::Corrupted: reason = extractStatusDescription(ExtractStatus::Corrupt); break;
        case ContentCategory::NoText: reason = extractStatusDescription(ExtractStatus::Empty); break;
        case ContentCategory::ImageOnly: reason = extractStatusDescription(ExtractStatus::ImageOnly); break;
        case ContentCategory::TooLarge: reason = extractStatusDescription(ExtractStatus::TooLarge); break;
        case ContentCategory::Binary: reason = extractStatusDescription(ExtractStatus::Binary); break;
    }
    if (!detail.empty() && category != ContentCategory::Extracted && category != ContentCategory::Unsupported) reason += " (" + detail + ")";
    return reason;
}

bool queryContentCounts(Database& db, std::int64_t rootId, ContentCounts& counts,
                        std::map<std::string, ExtensionCounts>* byExtension, std::string& error) {
    counts = ContentCounts{};
    if (byExtension != nullptr) byExtension->clear();
    Statement statement;
    if (!statement.prepare(db, rowsQuery(db, rootId != 0)) || (rootId != 0 && !statement.bindInt64(1, rootId))) {
        error = "cannot read the index: " + db.lastError();
        return false;
    }
    Statement::Step step;
    while ((step = statement.step()) == Statement::Step::Row) {
        const std::string path = statement.columnText(0);
        const int status = static_cast<int>(statement.columnInt64(2));
        const int extractor = static_cast<int>(statement.columnInt64(3));
        const ContentCategory category = categorizeContent(path, statement.columnInt64(1), status, extractor);
        add(counts, category);
        if (category == ContentCategory::Extracted && status != 0) {
            const ContentKind kind = contentKindFor(fs::u8path(path));
            if (extractor < extractorVersion(kind)) ++counts.extractedByOlderReader;
        }
        if (byExtension != nullptr) {
            ExtensionCounts& entry = (*byExtension)[extensionOf(path)];
            ++entry.files;
            add(entry.counts, category);
        }
    }
    if (step == Statement::Step::Error) {
        error = "cannot read the index: " + db.lastError();
        return false;
    }
    return true;
}

bool listContentFiles(Database& db, std::int64_t rootId, ContentCategory category, std::size_t limit,
                      std::vector<ContentFile>& files, std::string& error) {
    files.clear();
    Statement statement;
    if (!statement.prepare(db, rowsQuery(db, rootId != 0) + " ORDER BY d.path") ||
        (rootId != 0 && !statement.bindInt64(1, rootId))) {
        error = "cannot read the index: " + db.lastError();
        return false;
    }
    Statement::Step step;
    while (files.size() < limit && (step = statement.step()) == Statement::Step::Row) {
        const std::string path = statement.columnText(0);
        const ContentCategory found =
            categorizeContent(path, statement.columnInt64(1), static_cast<int>(statement.columnInt64(2)), static_cast<int>(statement.columnInt64(3)));
        if (found != category) continue;
        files.push_back({path, found, explainContent(path, found, statement.columnText(4))});
    }
    return true;
}

void addContentNotes(Database& db, std::vector<FileHit>& hits) {
    if (!db.hasContentStatus()) return;
    Statement note;
    if (!note.prepare(db, "SELECT d.token_count, COALESCE(c.status, 0), COALESCE(c.extractor, 0), COALESCE(c.detail, '') "
                          "FROM documents d LEFT JOIN content_status c ON c.doc_id = d.id WHERE d.path = ?1")) {
        return;
    }
    for (FileHit& hit : hits) {
        if (hit.kind != MatchKind::Filename) continue;
        note.reset();
        if (!note.bindText(1, hit.path) || note.step() != Statement::Step::Row) continue;
        const ContentCategory category = categorizeContent(hit.path, note.columnInt64(0), static_cast<int>(note.columnInt64(1)),
                                                           static_cast<int>(note.columnInt64(2)));
        if (category == ContentCategory::Extracted || category == ContentCategory::Unsupported) continue;
        hit.contentNote = contentCategoryName(category);
        const std::string detail = note.columnText(3);
        if (!detail.empty()) hit.contentNote += ": " + detail;
    }
}

}  // namespace flux
