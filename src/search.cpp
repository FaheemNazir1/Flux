#include "flux/search.hpp"

#include "extract_internal.hpp"
#include "flux/extract.hpp"
#include "flux/path_utils.hpp"
#include "flux/tokenizer.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace flux {

bool searchIndex(Database& db, std::string_view query, std::size_t limit,
                 std::vector<SearchHit>& hits, std::string& error, std::int64_t rootId) {
    hits.clear();
    if (rootId != 0 && db.schemaVersion() < 2) {
        error = "this database has no root registry (schema 1); it cannot be filtered by root";
        return false;
    }

    std::vector<std::string> terms = tokenize(query);
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    if (terms.empty()) {
        error = "query has no searchable terms";
        return false;
    }

    std::string sql =
        "SELECT d.path, SUM(p.frequency) AS score "
        "FROM terms t "
        "JOIN postings p ON p.term_id = t.id "
        "JOIN documents d ON d.id = p.doc_id "
        "WHERE t.term IN (?";
    for (std::size_t i = 1; i < terms.size(); ++i) {
        sql += ", ?";
    }
    sql += ")";
    if (rootId != 0) {
        sql += " AND d.root_id = ?";
    }
    sql += " GROUP BY p.doc_id HAVING COUNT(DISTINCT t.id) = ? ORDER BY score DESC, d.path LIMIT ?";

    Statement statement;
    if (!statement.prepare(db, sql)) {
        error = "cannot prepare search: " + db.lastError();
        return false;
    }
    int index = 1;
    for (const std::string& term : terms) {
        if (!statement.bindText(index++, term)) {
            error = "cannot bind search term: " + db.lastError();
            return false;
        }
    }
    if (rootId != 0 && !statement.bindInt64(index++, rootId)) {
        error = "cannot bind search root: " + db.lastError();
        return false;
    }
    if (!statement.bindInt64(index++, static_cast<std::int64_t>(terms.size())) ||
        !statement.bindInt64(index, static_cast<std::int64_t>(limit))) {
        error = "cannot bind search parameters: " + db.lastError();
        return false;
    }

    Statement::Step step;
    while ((step = statement.step()) == Statement::Step::Row) {
        hits.push_back({statement.columnText(0), statement.columnInt64(1)});
    }
    if (step == Statement::Step::Error) {
        error = "search failed: " + db.lastError();
        hits.clear();
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------ filename search

namespace {

constexpr std::size_t kNameContentLookups = 1000;  // best name matches checked for a content match too
constexpr std::size_t kSnippetReadBytes = 2 * 1024 * 1024;
constexpr std::size_t kSnippetContext = 50;  // bytes kept on each side of the match

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool isAlnumAscii(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
bool isAscii(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}
std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = lowerAscii(c);
    return out;
}

// The words of the query as they are compared with file names: split on whitespace, ASCII punctuation
// trimmed from both ends, lowercased (ASCII only; other letters must match exactly), duplicates dropped.
std::vector<std::string> nameChunks(std::string_view query) {
    std::vector<std::string> chunks;
    std::string current;
    const auto trim = [](char c) {
        return c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '"' || c == '\'' || c == '(' ||
               c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '<' || c == '>' || c == '*';
    };
    const auto flush = [&] {
        std::size_t b = 0, e = current.size();
        while (b < e && trim(current[b])) ++b;
        while (e > b && trim(current[e - 1])) --e;
        const bool hasLetter = std::any_of(current.begin() + static_cast<std::ptrdiff_t>(b), current.begin() + static_cast<std::ptrdiff_t>(e),
                                           [](char c) { return isAlnumAscii(c) || static_cast<unsigned char>(c) >= 0x80; });
        if (e > b && hasLetter) {  // a chunk of only punctuation ("." or "-") searches for nothing
            std::string chunk = lowered(std::string_view(current).substr(b, e - b));
            if (std::find(chunks.begin(), chunks.end(), chunk) == chunks.end()) chunks.push_back(std::move(chunk));
        }
        current.clear();
    };
    for (const char c : query) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') flush();
        else current.push_back(c);
    }
    flush();
    return chunks;
}

// The pieces of a chunk that must occur in a name: its alphanumeric runs for plain ASCII text (so
// "tax-return" also finds tax_return.txt), otherwise the chunk itself.
std::vector<std::string> chunkPieces(const std::string& chunk) {
    if (isAscii(chunk)) {
        std::vector<std::string> pieces = tokenize(chunk);
        if (!pieces.empty()) return pieces;
    }
    return {chunk};
}

bool chunkInName(const std::string& name, const std::string& chunk) {
    if (name.find(chunk) != std::string::npos) return true;
    for (const std::string& piece : chunkPieces(chunk)) {
        if (name.find(piece) == std::string::npos) return false;
    }
    return true;
}

bool wholeWord(const std::string& name, const std::string& chunk) {
    for (const std::string& piece : chunkPieces(chunk)) {
        bool found = false;
        for (std::size_t at = name.find(piece); at != std::string::npos; at = name.find(piece, at + 1)) {
            const bool before = at == 0 || !isAlnumAscii(name[at - 1]);
            const bool after = at + piece.size() >= name.size() || !isAlnumAscii(name[at + piece.size()]);
            if (before && after) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

// 0: the name does not contain every word; otherwise 1 (substring) .. 4 (exact name or name without extension).
int rankName(const std::string& name, const std::vector<std::string>& chunks) {
    for (const std::string& chunk : chunks) {
        if (!chunkInName(name, chunk)) return 0;
    }
    std::string joined;
    for (const std::string& chunk : chunks) joined += (joined.empty() ? "" : " ") + chunk;
    const std::size_t dot = name.rfind('.');
    const std::string stem = dot != std::string::npos && dot > 0 ? name.substr(0, dot) : name;
    if (name == joined || stem == joined) return 4;
    if (std::all_of(chunks.begin(), chunks.end(), [&](const std::string& c) { return wholeWord(name, c); })) return 3;
    if (name.compare(0, chunks.front().size(), chunks.front()) == 0) return 2;
    return 1;
}

std::string baseName(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

bool searchFiles(Database& db, std::string_view query, std::size_t limit, std::vector<FileHit>& hits,
                 std::string& error, std::int64_t rootId) {
    hits.clear();
    if (rootId != 0 && db.schemaVersion() < 2) {
        error = "this database has no root registry (schema 1); it cannot be filtered by root";
        return false;
    }
    std::vector<std::string> terms = tokenize(query);
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    const std::vector<std::string> chunks = nameChunks(query);
    if (chunks.empty()) {
        error = "query has no searchable terms";
        return false;
    }

    std::unordered_map<std::string, FileHit> found;

    // 1. Name matches. SQLite narrows by "the path contains every piece" (a superset, compared without
    //    case); the exact test is on the file name alone.
    struct NameCandidate {
        std::int64_t id;
        int rank;
        FileHit hit;
    };
    std::vector<NameCandidate> named;
    {
        std::vector<std::string> pieces;
        for (const std::string& chunk : chunks) {
            for (std::string& piece : chunkPieces(chunk)) pieces.push_back(std::move(piece));
        }
        std::sort(pieces.begin(), pieces.end());
        pieces.erase(std::unique(pieces.begin(), pieces.end()), pieces.end());
        std::string sql = "SELECT id, path, size_bytes, mtime_ns FROM documents WHERE 1";
        for (std::size_t i = 0; i < pieces.size(); ++i) sql += " AND path LIKE ? ESCAPE '\\'";
        if (rootId != 0) sql += " AND root_id = ?";
        Statement statement;
        if (!statement.prepare(db, sql)) {
            error = "cannot prepare filename search: " + db.lastError();
            return false;
        }
        int index = 1;
        bool bound = true;
        for (const std::string& piece : pieces) {
            std::string pattern = "%";
            for (const char c : piece) {
                if (c == '%' || c == '_' || c == '\\') pattern.push_back('\\');
                pattern.push_back(c);
            }
            pattern.push_back('%');
            bound = bound && statement.bindText(index++, pattern);
        }
        if (rootId != 0) bound = bound && statement.bindInt64(index, rootId);
        if (!bound) {
            error = "cannot bind filename search parameters: " + db.lastError();
            return false;
        }
        Statement::Step step;
        while ((step = statement.step()) == Statement::Step::Row) {
            const std::string path = statement.columnText(1);
            const int rank = rankName(lowered(baseName(path)), chunks);
            if (rank == 0) continue;
            NameCandidate candidate;
            candidate.id = statement.columnInt64(0);
            candidate.rank = rank;
            candidate.hit.path = path;
            candidate.hit.kind = MatchKind::Filename;
            candidate.hit.nameRank = rank;
            candidate.hit.sizeBytes = static_cast<std::uint64_t>(statement.columnInt64(2));
            candidate.hit.modifiedUnix = fileClockToUnixSeconds(statement.columnInt64(3));
            named.push_back(std::move(candidate));
        }
        if (step == Statement::Step::Error) {
            error = "filename search failed: " + db.lastError();
            return false;
        }
    }

    // 2. Which of the best name matches ALSO contain the words (their content score breaks ties and marks
    //    them "name + content"). Bounded: only the first kNameContentLookups by (rank, path) are checked.
    if (!terms.empty() && !named.empty()) {
        std::sort(named.begin(), named.end(), [](const NameCandidate& a, const NameCandidate& b) {
            return a.rank != b.rank ? a.rank > b.rank : a.hit.path < b.hit.path;
        });
        const std::size_t checked = std::min(named.size(), kNameContentLookups);
        std::unordered_map<std::int64_t, std::size_t> position;
        for (std::size_t i = 0; i < checked; ++i) position[named[i].id] = i;
        constexpr std::size_t kIdsPerQuery = 400;
        for (std::size_t from = 0; from < checked; from += kIdsPerQuery) {
            const std::size_t to = std::min(checked, from + kIdsPerQuery);
            std::string sql =
                "SELECT p.doc_id, SUM(p.frequency) FROM terms t JOIN postings p ON p.term_id = t.id "
                "WHERE t.term IN (?";
            for (std::size_t i = 1; i < terms.size(); ++i) sql += ", ?";
            sql += ") AND p.doc_id IN (?";
            for (std::size_t i = from + 1; i < to; ++i) sql += ", ?";
            sql += ") GROUP BY p.doc_id HAVING COUNT(DISTINCT t.id) = ?";
            Statement statement;
            if (!statement.prepare(db, sql)) {
                error = "cannot prepare search: " + db.lastError();
                return false;
            }
            int index = 1;
            bool bound = true;
            for (const std::string& term : terms) bound = bound && statement.bindText(index++, term);
            for (std::size_t i = from; i < to; ++i) bound = bound && statement.bindInt64(index++, named[i].id);
            bound = bound && statement.bindInt64(index, static_cast<std::int64_t>(terms.size()));
            if (!bound) {
                error = "cannot bind search parameters: " + db.lastError();
                return false;
            }
            Statement::Step step;
            while ((step = statement.step()) == Statement::Step::Row) {
                const auto it = position.find(statement.columnInt64(0));
                if (it == position.end()) continue;
                named[it->second].hit.contentScore = statement.columnInt64(1);
                named[it->second].hit.kind = MatchKind::Both;
            }
            if (step == Statement::Step::Error) {
                error = "search failed: " + db.lastError();
                return false;
            }
        }
    }
    for (NameCandidate& candidate : named) found.emplace(candidate.hit.path, std::move(candidate.hit));

    // 3. Content matches (the same rule and order as searchIndex): the best `limit` of them. Enough,
    //    because name matches always rank first and fill at most `limit` places.
    if (!terms.empty()) {
        std::string sql =
            "SELECT d.path, SUM(p.frequency) AS score, d.size_bytes, d.mtime_ns "
            "FROM terms t JOIN postings p ON p.term_id = t.id JOIN documents d ON d.id = p.doc_id "
            "WHERE t.term IN (?";
        for (std::size_t i = 1; i < terms.size(); ++i) sql += ", ?";
        sql += ")";
        if (rootId != 0) sql += " AND d.root_id = ?";
        sql += " GROUP BY p.doc_id HAVING COUNT(DISTINCT t.id) = ? ORDER BY score DESC, d.path LIMIT ?";
        Statement statement;
        if (!statement.prepare(db, sql)) {
            error = "cannot prepare search: " + db.lastError();
            return false;
        }
        int index = 1;
        bool bound = true;
        for (const std::string& term : terms) bound = bound && statement.bindText(index++, term);
        if (rootId != 0) bound = bound && statement.bindInt64(index++, rootId);
        bound = bound && statement.bindInt64(index++, static_cast<std::int64_t>(terms.size())) &&
                statement.bindInt64(index, static_cast<std::int64_t>(limit));
        if (!bound) {
            error = "cannot bind search parameters: " + db.lastError();
            return false;
        }
        Statement::Step step;
        while ((step = statement.step()) == Statement::Step::Row) {
            std::string path = statement.columnText(0);
            if (found.count(path) > 0) continue;  // already listed as a name match
            FileHit hit;
            hit.path = path;
            hit.contentScore = statement.columnInt64(1);
            hit.sizeBytes = static_cast<std::uint64_t>(statement.columnInt64(2));
            hit.modifiedUnix = fileClockToUnixSeconds(statement.columnInt64(3));
            hit.kind = MatchKind::Content;
            found.emplace(std::move(path), std::move(hit));
        }
        if (step == Statement::Step::Error) {
            error = "search failed: " + db.lastError();
            return false;
        }
    }

    hits.reserve(found.size());
    for (auto& entry : found) hits.push_back(std::move(entry.second));
    std::sort(hits.begin(), hits.end(), [](const FileHit& a, const FileHit& b) {
        if (a.nameRank != b.nameRank) return a.nameRank > b.nameRank;
        if (a.contentScore != b.contentScore) return a.contentScore > b.contentScore;
        return a.path < b.path;
    });
    if (hits.size() > limit) hits.resize(limit);
    return true;
}

// ---------------------------------------------------------------------------------------- snippets

namespace {

struct Snippet {
    std::string text;
    std::string location;
};

Snippet snippetFor(const std::string& path, const std::unordered_set<std::string>& terms) {
    const std::filesystem::path file = std::filesystem::u8path(path);
    const ContentKind kind = contentKindFor(file);
    std::string text;
    std::vector<TextSegment> segments;
    bool plain = false;
    if (kind == ContentKind::None || kind == ContentKind::PlainText) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return {};
        text.assign(kSnippetReadBytes, '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(in.gcount()));
        // Plain UTF-8 / ASCII is used as it is. UTF-16 and Latin-1 files are converted by the extractor, as at indexing time.
        const bool utf16Bom = text.size() >= 2 && ((static_cast<unsigned char>(text[0]) == 0xFF && static_cast<unsigned char>(text[1]) == 0xFE) ||
                                                   (static_cast<unsigned char>(text[0]) == 0xFE && static_cast<unsigned char>(text[1]) == 0xFF));
        plain = !utf16Bom && text.find('\0') == std::string::npos && extract_detail::isValidUtf8(text);
        if (!plain && kind == ContentKind::None) return {};
    }
    if (!plain) {
        ExtractLimits limits;
        limits.wantSegments = true;
        limits.timeBudget = std::chrono::milliseconds(8000);
        ExtractResult extracted = extractContent(file, limits);
        if (!statusHasText(extracted.status)) return {};
        text = std::move(extracted.text);
        segments = std::move(extracted.segments);
    }

    for (std::size_t i = 0; i < text.size();) {
        if (!isAlnumAscii(text[i])) { ++i; continue; }
        std::size_t j = i;
        std::string word;
        while (j < text.size() && isAlnumAscii(text[j])) word.push_back(lowerAscii(text[j++]));
        if (terms.count(word) == 0) { i = j; continue; }

        std::size_t from = i > kSnippetContext ? i - kSnippetContext : 0;
        std::size_t to = std::min(text.size(), j + kSnippetContext);
        const auto continuation = [&](std::size_t at) {
            return at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80;
        };
        while (from < i && continuation(from)) ++from;
        while (to > j && continuation(to)) --to;
        std::string out;
        bool space = true;  // collapses runs of whitespace and trims the start
        for (std::size_t k = from; k < to; ++k) {
            const unsigned char c = static_cast<unsigned char>(text[k]);
            if (c < 0x20 || c == ' ') {
                if (!space) out.push_back(' ');
                space = true;
            } else {
                out.push_back(static_cast<char>(c));
                space = false;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return {(from > 0 ? "..." : "") + out + (to < text.size() ? "..." : ""), segmentLabelAt(segments, i)};
    }
    return {};
}

}  // namespace

void addSnippets(std::vector<FileHit>& hits, std::string_view query) {
    const std::vector<std::string> words = tokenize(query);
    const std::unordered_set<std::string> terms(words.begin(), words.end());
    if (terms.empty()) return;
    for (FileHit& hit : hits) {
        if (hit.contentScore > 0) {
            Snippet found = snippetFor(hit.path, terms);
            hit.snippet = std::move(found.text);
            hit.location = std::move(found.location);
        }
    }
}

std::string formatFileSize(std::uint64_t bytes) {
    char buffer[40];
    if (bytes >= (1ull << 30)) std::snprintf(buffer, sizeof buffer, "%.1f GiB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= (1u << 20)) std::snprintf(buffer, sizeof buffer, "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024) std::snprintf(buffer, sizeof buffer, "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    else std::snprintf(buffer, sizeof buffer, "%llu bytes", static_cast<unsigned long long>(bytes));
    return buffer;
}

std::string formatLocalDateTime(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) return {};
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm local = {};
#ifdef _WIN32
    if (localtime_s(&local, &t) != 0) return {};
#else
    if (localtime_r(&t, &local) == nullptr) return {};
#endif
    char buffer[32];
    return std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M", &local) > 0 ? std::string(buffer) : std::string();
}

std::string fileTypeLabel(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) return "File";
    std::string ext(name.substr(dot + 1));
    for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext + " file";
}

}  // namespace flux
