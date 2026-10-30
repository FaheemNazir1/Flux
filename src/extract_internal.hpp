#pragma once
// Shared pieces of the content extractors. Not part of the public API (see flux/extract.hpp).

#include "flux/extract.hpp"

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace flux::extract_detail {

// ---------------------------------------------------------------------------------------------- budget

// Wall-clock and cancellation guard for loops over untrusted input. Call expired() freely: the clock is
// read once every few hundred calls.
class Budget {
public:
    explicit Budget(const ExtractLimits& limits)
        : end_(std::chrono::steady_clock::now() + limits.timeBudget), cancel_(limits.cancel) {}
    bool expired() {
        if (done_) return true;
        if ((++tick_ & 0xFF) != 0) return false;
        if ((cancel_ != nullptr && cancel_->load()) || std::chrono::steady_clock::now() > end_) done_ = true;
        return done_;
    }
    bool cancelled() const { return cancel_ != nullptr && cancel_->load(); }

private:
    std::chrono::steady_clock::time_point end_;
    const std::atomic<bool>* cancel_;
    unsigned tick_ = 0;
    bool done_ = false;
};

// ------------------------------------------------------------------------------------------------ text

void appendUtf8(std::string& out, std::uint32_t codePoint);
// Converts UTF-16 (host-independent byte order given by `bigEndian`) to UTF-8.
std::string utf16ToUtf8(const unsigned char* data, std::size_t size, bool bigEndian);
bool isValidUtf8(std::string_view text);
std::string latin1ToUtf8(std::string_view text);  // Windows-1252 flavoured
// Turns the bytes of a text file into UTF-8: strips a BOM, converts UTF-16, converts invalid UTF-8
// from Latin-1. Returns false if the bytes are binary (NUL bytes that are not UTF-16 text).
bool decodeTextBytes(std::string_view raw, std::string& utf8);

// Collects extracted text under a size limit and (optionally) labels.
class TextSink {
public:
    explicit TextSink(const ExtractLimits& limits) : max_(limits.maxTextBytes), wantSegments_(limits.wantSegments) {}
    void append(std::string_view text);
    void append(char c) { append(std::string_view(&c, 1)); }
    // Separators that never double up.
    void space();
    void newline();
    void segment(const std::string& label);  // label for the text appended from now on
    // A label for text that was appended earlier, starting at `offset` (kept in offset order).
    void segmentAt(std::uint64_t offset, const std::string& label);
    bool full() const { return truncated_; }
    bool truncated() const { return truncated_; }
    bool hasText() const;
    std::size_t size() const { return text_.size(); }
    char last() const { return text_.empty() ? '\n' : text_.back(); }
    std::string& text() { return text_; }
    std::vector<TextSegment>& segments() { return segments_; }

private:
    std::size_t max_;
    bool wantSegments_;
    bool truncated_ = false;
    std::string text_;
    std::vector<TextSegment> segments_;
};

// ----------------------------------------------------------------------------------------- byte source

// Random access to the bytes of a file or a buffer.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual std::uint64_t size() const = 0;
    virtual bool read(std::uint64_t offset, void* buffer, std::size_t count) const = 0;
};

class FileSource : public ByteSource {
public:
    explicit FileSource(const std::filesystem::path& path);
    bool ok() const { return ok_; }
    std::uint64_t size() const override { return size_; }
    bool read(std::uint64_t offset, void* buffer, std::size_t count) const override;

private:
    mutable std::ifstream in_;
    std::uint64_t size_ = 0;
    bool ok_ = false;
};

class MemorySource : public ByteSource {
public:
    explicit MemorySource(const std::string& bytes) : bytes_(bytes) {}
    std::uint64_t size() const override { return bytes_.size(); }
    bool read(std::uint64_t offset, void* buffer, std::size_t count) const override;

private:
    const std::string& bytes_;
};

// ----------------------------------------------------------------------------------------------- inflate

enum class InflateStatus { Ok, Corrupt, TooLarge };
// Raw DEFLATE (RFC 1951). Appends to `out`; stops with TooLarge when `out` would exceed `maxOut`.
// `consumed` (optional) receives the number of input bytes the deflate stream occupied.
InflateStatus inflateRaw(const unsigned char* data, std::size_t size, std::string& out, std::size_t maxOut,
                         std::size_t* consumed = nullptr);
// zlib stream (RFC 1950): 2-byte header, deflate data, Adler-32 (not verified).
InflateStatus inflateZlib(const unsigned char* data, std::size_t size, std::string& out, std::size_t maxOut);

// -------------------------------------------------------------------------------------------------- zip

struct ZipEntry {
    std::string name;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t size = 0;
    std::uint64_t localOffset = 0;
    bool isDirectory() const { return !name.empty() && name.back() == '/'; }
};

enum class ZipRead { Ok, Corrupt, TooLarge, Encrypted, Unsupported };

class ZipArchive {
public:
    // Reads the central directory. Fails (with `error`) if this is not a readable ZIP file.
    bool open(const ByteSource& source, std::string& error);
    const std::vector<ZipEntry>& entries() const { return entries_; }
    const ZipEntry* find(std::string_view name) const;
    // Decompresses one entry (at most `maxOut` bytes).
    ZipRead read(const ZipEntry& entry, std::string& out, std::size_t maxOut) const;

private:
    const ByteSource* source_ = nullptr;
    std::vector<ZipEntry> entries_;
};

// --------------------------------------------------------------------------------------------------- xml

// A forgiving, non-validating XML/HTML tokenizer: start tags (with their raw attribute text), end tags
// and decoded character data. Comments, processing instructions and DOCTYPE are skipped; CDATA is text.
// Only the five predefined entities and numeric references are expanded (no DTD, so nothing external is
// ever fetched and nothing can expand into more than it says).
struct XmlEvent {
    enum class Kind { End, Start, Close, Text } kind = Kind::End;
    std::string_view name;   // Start / Close: tag name as written (prefix included)
    std::string_view attrs;  // Start: raw attribute text
    bool selfClosing = false;
    std::string text;        // Text: decoded
};

class XmlScanner {
public:
    explicit XmlScanner(std::string_view data) : data_(data) {}
    bool next(XmlEvent& event);  // false at the end

private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

// Value of attribute `name` in a raw attribute string ("" if absent); entities expanded.
std::string xmlAttr(std::string_view attrs, std::string_view name);
bool xmlHasAttr(std::string_view attrs, std::string_view name);
std::string decodeEntities(std::string_view text);
// The part of a tag name after the prefix: "w:t" -> "t".
inline std::string_view localName(std::string_view name) {
    const std::size_t colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

// ------------------------------------------------------------------------------------------- extractors

// Each fills `sink` and returns a status; detail/units go to `result`.
struct Context {
    const ExtractLimits& limits;
    Budget& budget;
    TextSink& sink;
    ExtractResult& result;
};

ExtractStatus extractPlainText(const ByteSource& source, Context& ctx);
ExtractStatus extractHtml(const ByteSource& source, Context& ctx);
ExtractStatus extractXml(const ByteSource& source, Context& ctx);
ExtractStatus extractRtf(const ByteSource& source, Context& ctx);
ExtractStatus extractEml(const ByteSource& source, Context& ctx);
ExtractStatus extractDocx(const ByteSource& source, Context& ctx);
ExtractStatus extractPptx(const ByteSource& source, Context& ctx);
ExtractStatus extractXlsx(const ByteSource& source, Context& ctx);
ExtractStatus extractOdf(const ByteSource& source, Context& ctx);
ExtractStatus extractEpub(const ByteSource& source, Context& ctx);
ExtractStatus extractZip(const ByteSource& source, Context& ctx, int depth);
ExtractStatus extractPdf(const ByteSource& source, Context& ctx);
// Runs the extractor for `kind` (used by ZIP archives for their entries).
ExtractStatus dispatchExtract(ContentKind kind, const ByteSource& source, Context& ctx, int depth);

// Text of an HTML/XHTML buffer appended to the sink (shared by HTML, EPUB and e-mail).
void htmlToText(std::string_view html, TextSink& sink, Budget& budget);
// Reads a whole source into memory (bounded by `maxBytes`).
bool readAll(const ByteSource& source, std::string& out, std::uint64_t maxBytes);

}  // namespace flux::extract_detail
