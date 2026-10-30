// Text-like formats: plain text (UTF-8/UTF-16/Latin-1), HTML, XML, RTF and e-mail messages; plus the
// small text utilities every extractor shares.
#include "extract_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace flux::extract_detail {

// ------------------------------------------------------------------------------------------- utilities

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string utf16ToUtf8(const unsigned char* data, std::size_t size, bool bigEndian) {
    std::string out;
    out.reserve(size);
    for (std::size_t i = 0; i + 1 < size; i += 2) {
        std::uint32_t unit = bigEndian ? ((data[i] << 8) | data[i + 1]) : ((data[i + 1] << 8) | data[i]);
        if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < size) {
            const std::uint32_t low = bigEndian ? ((data[i + 2] << 8) | data[i + 3]) : ((data[i + 3] << 8) | data[i + 2]);
            if (low >= 0xDC00 && low < 0xE000) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        appendUtf8(out, unit);
    }
    return out;
}

bool isValidUtf8(std::string_view text) {
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t extra;
        if (c < 0x80) { ++i; continue; }
        else if (c >= 0xC2 && c <= 0xDF) extra = 1;
        else if (c >= 0xE0 && c <= 0xEF) extra = 2;
        else if (c >= 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        if (i + extra >= n) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string latin1ToUtf8(std::string_view text) {
    static const std::uint16_t cp1252[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                             0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                             0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x80) out.push_back(ch);
        else if (c < 0xA0) appendUtf8(out, cp1252[c - 0x80]);
        else appendUtf8(out, c);
    }
    return out;
}

bool decodeTextBytes(std::string_view raw, std::string& utf8) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(raw.data());
    const std::size_t n = raw.size();
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        utf8.assign(raw.substr(3));
        return utf8.find('\0') == std::string::npos;
    }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { utf8 = utf16ToUtf8(p + 2, n - 2, false); return true; }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { utf8 = utf16ToUtf8(p + 2, n - 2, true); return true; }
    if (raw.find('\0') != std::string_view::npos) {
        // UTF-16 without a BOM: mostly ASCII text has a zero in every second byte.
        const std::size_t sample = std::min<std::size_t>(n & ~std::size_t{1}, 4096);
        std::size_t zerosEven = 0, zerosOdd = 0;
        for (std::size_t i = 0; i + 1 < sample; i += 2) {
            if (p[i] == 0) ++zerosEven;
            if (p[i + 1] == 0) ++zerosOdd;
        }
        const std::size_t pairs = sample / 2;
        if (pairs >= 4 && zerosOdd * 10 >= pairs * 3 && zerosEven * 20 <= pairs) { utf8 = utf16ToUtf8(p, n, false); return true; }
        if (pairs >= 4 && zerosEven * 10 >= pairs * 3 && zerosOdd * 20 <= pairs) { utf8 = utf16ToUtf8(p, n, true); return true; }
        return false;
    }
    if (isValidUtf8(raw)) utf8.assign(raw);
    else utf8 = latin1ToUtf8(raw);
    return true;
}

void TextSink::append(std::string_view text) {
    if (truncated_ || text.empty()) return;
    if (text_.size() + text.size() > max_) {
        std::size_t room = max_ > text_.size() ? max_ - text_.size() : 0;
        while (room > 0 && room < text.size() && (static_cast<unsigned char>(text[room]) & 0xC0) == 0x80) --room;
        text_.append(text.data(), room);
        truncated_ = true;
        return;
    }
    text_.append(text.data(), text.size());
}

void TextSink::space() {
    if (text_.empty()) return;
    const char c = text_.back();
    if (c != ' ' && c != '\n' && c != '\t') append(' ');
}

void TextSink::newline() {
    if (text_.empty()) return;
    while (!text_.empty() && text_.back() == ' ') text_.pop_back();
    if (!text_.empty() && text_.back() != '\n') append('\n');
}

void TextSink::segment(const std::string& label) {
    if (!wantSegments_ || truncated_) return;
    if (!segments_.empty() && segments_.back().offset == text_.size()) segments_.back().label = label;
    else segments_.push_back({text_.size(), label});
}

void TextSink::segmentAt(std::uint64_t offset, const std::string& label) {
    if (!wantSegments_) return;
    auto it = segments_.begin();
    while (it != segments_.end() && it->offset < offset) ++it;
    if (it != segments_.end() && it->offset == offset) it->label = label;
    else segments_.insert(it, {offset, label});
}

bool TextSink::hasText() const {
    for (const char ch : text_) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c >= 0x80) return true;
    }
    return false;
}

// -------------------------------------------------------------------------------------------------- xml

namespace {

bool startsWith(std::string_view text, std::size_t at, std::string_view what) {
    return text.compare(at, what.size(), what) == 0;
}

char lowerChar(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerChar(a[i]) != lowerChar(b[i])) return false;
    }
    return true;
}

// Case-insensitive find of an ASCII needle.
std::size_t ifind(std::string_view hay, std::string_view needle, std::size_t from) {
    if (needle.empty() || hay.size() < needle.size()) return std::string_view::npos;
    for (std::size_t i = from; i + needle.size() <= hay.size(); ++i) {
        if (lowerChar(hay[i]) == needle[0] && iequals(hay.substr(i, needle.size()), needle)) return i;
    }
    return std::string_view::npos;
}

struct NamedEntity { const char* name; std::uint32_t cp; };
const NamedEntity kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE},
    {"trade", 0x2122}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022}, {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5},
    {"cent", 0xA2}, {"sect", 0xA7}, {"deg", 0xB0}, {"plusmn", 0xB1}, {"times", 0xD7}, {"divide", 0xF7}, {"middot", 0xB7},
    {"laquo", 0xAB}, {"raquo", 0xBB}, {"eacute", 0xE9}, {"egrave", 0xE8}, {"agrave", 0xE0}, {"aacute", 0xE1},
    {"auml", 0xE4}, {"ouml", 0xF6}, {"uuml", 0xFC}, {"szlig", 0xDF}, {"ntilde", 0xF1}, {"ccedil", 0xE7},
    {"Auml", 0xC4}, {"Ouml", 0xD6}, {"Uuml", 0xDC}, {"Eacute", 0xC9}, {"iexcl", 0xA1}, {"iquest", 0xBF},
};

}  // namespace

std::string decodeEntities(std::string_view text) {
    if (text.find('&') == std::string_view::npos) return std::string(text);
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') { out.push_back(text[i]); continue; }
        const std::size_t semi = text.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 10) { out.push_back('&'); continue; }
        const std::string_view name = text.substr(i + 1, semi - i - 1);
        std::uint32_t cp = 0;
        bool known = false;
        if (!name.empty() && name[0] == '#') {
            const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            const std::string digits(name.substr(hex ? 2 : 1));
            if (!digits.empty() && digits.size() <= 8 && digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") == std::string::npos) {
                cp = static_cast<std::uint32_t>(std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10));
                known = cp != 0;
            }
        } else {
            for (const NamedEntity& entity : kEntities) {
                if (name == entity.name) { cp = entity.cp; known = true; break; }
            }
        }
        if (!known) { out.push_back('&'); continue; }
        appendUtf8(out, cp);
        i = semi;
    }
    return out;
}

bool XmlScanner::next(XmlEvent& event) {
    event = XmlEvent{};
    while (pos_ < data_.size()) {
        if (data_[pos_] != '<') {
            const std::size_t end = data_.find('<', pos_);
            const std::size_t stop = end == std::string_view::npos ? data_.size() : end;
            event.kind = XmlEvent::Kind::Text;
            event.text = decodeEntities(data_.substr(pos_, stop - pos_));
            pos_ = stop;
            return true;
        }
        if (startsWith(data_, pos_, "<!--")) {
            const std::size_t end = data_.find("-->", pos_ + 4);
            pos_ = end == std::string_view::npos ? data_.size() : end + 3;
            continue;
        }
        if (startsWith(data_, pos_, "<![CDATA[")) {
            const std::size_t end = data_.find("]]>", pos_ + 9);
            const std::size_t stop = end == std::string_view::npos ? data_.size() : end;
            event.kind = XmlEvent::Kind::Text;
            event.text.assign(data_.substr(pos_ + 9, stop - pos_ - 9));
            pos_ = end == std::string_view::npos ? data_.size() : end + 3;
            return true;
        }
        if (startsWith(data_, pos_, "<?")) {
            const std::size_t end = data_.find("?>", pos_ + 2);
            pos_ = end == std::string_view::npos ? data_.size() : end + 2;
            continue;
        }
        if (startsWith(data_, pos_, "<!")) {
            int depth = 0;
            std::size_t i = pos_ + 2;
            for (; i < data_.size(); ++i) {
                if (data_[i] == '[') ++depth;
                else if (data_[i] == ']') --depth;
                else if (data_[i] == '>' && depth <= 0) break;
            }
            pos_ = i < data_.size() ? i + 1 : data_.size();
            continue;
        }
        if (pos_ + 1 < data_.size() && data_[pos_ + 1] == '/') {
            const std::size_t end = data_.find('>', pos_ + 2);
            const std::size_t stop = end == std::string_view::npos ? data_.size() : end;
            std::string_view name = data_.substr(pos_ + 2, stop - pos_ - 2);
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.remove_suffix(1);
            event.kind = XmlEvent::Kind::Close;
            event.name = name;
            pos_ = end == std::string_view::npos ? data_.size() : end + 1;
            return true;
        }
        // Start tag.
        std::size_t i = pos_ + 1;
        const std::size_t nameStart = i;
        while (i < data_.size() && !std::isspace(static_cast<unsigned char>(data_[i])) && data_[i] != '>' && data_[i] != '/') ++i;
        if (i == nameStart) {  // a stray '<' in text
            event.kind = XmlEvent::Kind::Text;
            event.text = "<";
            ++pos_;
            return true;
        }
        event.kind = XmlEvent::Kind::Start;
        event.name = data_.substr(nameStart, i - nameStart);
        const std::size_t attrStart = i;
        char quote = 0;
        for (; i < data_.size(); ++i) {
            const char c = data_[i];
            if (quote != 0) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '>') break;
        }
        std::string_view attrs = data_.substr(attrStart, i - attrStart);
        while (!attrs.empty() && std::isspace(static_cast<unsigned char>(attrs.back()))) attrs.remove_suffix(1);
        if (!attrs.empty() && attrs.back() == '/') { event.selfClosing = true; attrs.remove_suffix(1); }
        event.attrs = attrs;
        pos_ = i < data_.size() ? i + 1 : data_.size();
        return true;
    }
    event.kind = XmlEvent::Kind::End;
    return false;
}

namespace {

// Walks `name="value"` pairs of a raw attribute string.
template <typename Fn>
void forEachAttr(std::string_view attrs, Fn&& fn) {
    std::size_t i = 0;
    while (i < attrs.size()) {
        while (i < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[i]))) ++i;
        const std::size_t nameStart = i;
        while (i < attrs.size() && attrs[i] != '=' && !std::isspace(static_cast<unsigned char>(attrs[i]))) ++i;
        const std::string_view name = attrs.substr(nameStart, i - nameStart);
        while (i < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[i]))) ++i;
        if (i >= attrs.size() || attrs[i] != '=') { if (name.empty()) ++i; continue; }
        ++i;
        while (i < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[i]))) ++i;
        std::string_view value;
        if (i < attrs.size() && (attrs[i] == '"' || attrs[i] == '\'')) {
            const char quote = attrs[i++];
            const std::size_t end = attrs.find(quote, i);
            const std::size_t stop = end == std::string_view::npos ? attrs.size() : end;
            value = attrs.substr(i, stop - i);
            i = end == std::string_view::npos ? attrs.size() : end + 1;
        } else {
            const std::size_t valueStart = i;
            while (i < attrs.size() && !std::isspace(static_cast<unsigned char>(attrs[i]))) ++i;
            value = attrs.substr(valueStart, i - valueStart);
        }
        if (!name.empty() && !fn(name, value)) return;
    }
}

}  // namespace

std::string xmlAttr(std::string_view attrs, std::string_view name) {
    std::string result;
    forEachAttr(attrs, [&](std::string_view n, std::string_view v) {
        if (n == name) { result = decodeEntities(v); return false; }
        return true;
    });
    return result;
}

bool xmlHasAttr(std::string_view attrs, std::string_view name) {
    bool found = false;
    forEachAttr(attrs, [&](std::string_view n, std::string_view) {
        if (n == name) { found = true; return false; }
        return true;
    });
    return found;
}

// Appends text with runs of whitespace collapsed to one space.
static void appendCollapsed(TextSink& sink, std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA0)) {
            sink.space();
            i += c == 0xC2 ? 2 : 1;
            continue;
        }
        std::size_t j = i;
        while (j < text.size()) {
            const unsigned char d = static_cast<unsigned char>(text[j]);
            if (d == ' ' || d == '\t' || d == '\n' || d == '\r' || d == '\f' || (d == 0xC2 && j + 1 < text.size() && static_cast<unsigned char>(text[j + 1]) == 0xA0)) break;
            ++j;
        }
        sink.append(text.substr(i, j - i));
        i = j;
        if (sink.full()) return;
    }
}

void htmlToText(std::string_view html, TextSink& sink, Budget& budget) {
    static const char* const kBlock[] = {"p", "div", "br", "li", "ul", "ol", "tr", "table", "h1", "h2", "h3", "h4", "h5", "h6",
                                         "section", "article", "header", "footer", "blockquote", "pre", "hr", "dt", "dd",
                                         "title", "body", "form", "nav", "aside", "main", "figure", "figcaption", "caption", "option"};
    std::size_t pos = 0;
    while (pos < html.size() && !sink.full()) {
        if (budget.expired()) return;
        if (html[pos] != '<') {
            const std::size_t end = html.find('<', pos);
            const std::size_t stop = end == std::string_view::npos ? html.size() : end;
            appendCollapsed(sink, decodeEntities(html.substr(pos, stop - pos)));
            pos = stop;
            continue;
        }
        if (startsWith(html, pos, "<!--")) {
            const std::size_t end = html.find("-->", pos + 4);
            pos = end == std::string_view::npos ? html.size() : end + 3;
            continue;
        }
        if (pos + 1 >= html.size() || !(std::isalpha(static_cast<unsigned char>(html[pos + 1])) || html[pos + 1] == '/')) {
            if (pos + 1 < html.size() && (html[pos + 1] == '!' || html[pos + 1] == '?')) {  // doctype, processing instruction
                const std::size_t end = html.find('>', pos);
                pos = end == std::string_view::npos ? html.size() : end + 1;
            } else {
                sink.append('<');
                ++pos;
            }
            continue;
        }
        std::size_t i = pos + 1;
        const bool closing = html[i] == '/';
        if (closing) ++i;
        const std::size_t nameStart = i;
        while (i < html.size() && (std::isalnum(static_cast<unsigned char>(html[i])) || html[i] == ':' || html[i] == '-')) ++i;
        std::string name(html.substr(nameStart, i - nameStart));
        for (char& c : name) c = lowerChar(c);
        // Find the end of the tag, respecting quoted attribute values.
        char quote = 0;
        for (; i < html.size(); ++i) {
            const char c = html[i];
            if (quote != 0) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '>') break;
        }
        pos = i < html.size() ? i + 1 : html.size();
        if (!closing && (name == "script" || name == "style" || name == "noscript" || name == "template")) {
            const std::string closeTag = "</" + name;
            const std::size_t end = ifind(html, closeTag, pos);
            if (end == std::string_view::npos) { pos = html.size(); break; }
            const std::size_t gt = html.find('>', end);
            pos = gt == std::string_view::npos ? html.size() : gt + 1;
            continue;
        }
        if (name == "td" || name == "th") sink.space();
        else {
            for (const char* block : kBlock) {
                if (name == block) { sink.newline(); break; }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------- the extractors

ExtractStatus extractPlainText(const ByteSource& source, Context& ctx) {
    std::string raw;
    if (source.size() > ctx.limits.maxPlainTextFileBytes) return ExtractStatus::TooLarge;
    if (!readAll(source, raw, ctx.limits.maxPlainTextFileBytes)) return ExtractStatus::Unreadable;
    std::string text;
    if (!decodeTextBytes(raw, text)) {
        ctx.result.detail = "the file contains binary data, not text";
        return ExtractStatus::Binary;
    }
    ctx.sink.append(text);
    return ExtractStatus::Ok;
}

ExtractStatus extractHtml(const ByteSource& source, Context& ctx) {
    std::string raw, text;
    if (source.size() > ctx.limits.maxPlainTextFileBytes) return ExtractStatus::TooLarge;
    if (!readAll(source, raw, ctx.limits.maxPlainTextFileBytes)) return ExtractStatus::Unreadable;
    if (!decodeTextBytes(raw, text)) { ctx.result.detail = "the file contains binary data, not text"; return ExtractStatus::Binary; }
    htmlToText(text, ctx.sink, ctx.budget);
    return ExtractStatus::Ok;
}

ExtractStatus extractXml(const ByteSource& source, Context& ctx) {
    std::string raw, text;
    if (source.size() > ctx.limits.maxPlainTextFileBytes) return ExtractStatus::TooLarge;
    if (!readAll(source, raw, ctx.limits.maxPlainTextFileBytes)) return ExtractStatus::Unreadable;
    if (!decodeTextBytes(raw, text)) { ctx.result.detail = "the file contains binary data, not text"; return ExtractStatus::Binary; }
    XmlScanner scanner(text);
    XmlEvent event;
    while (!ctx.sink.full() && scanner.next(event)) {
        if (ctx.budget.expired()) break;
        switch (event.kind) {
            case XmlEvent::Kind::Text: appendCollapsed(ctx.sink, event.text); break;
            case XmlEvent::Kind::Start:
                ctx.sink.space();
                forEachAttr(event.attrs, [&](std::string_view, std::string_view value) {
                    if (value.size() >= 2 && value.size() <= 400) {
                        appendCollapsed(ctx.sink, decodeEntities(value));
                        ctx.sink.space();
                    }
                    return !ctx.sink.full();
                });
                break;
            case XmlEvent::Kind::Close: ctx.sink.space(); break;
            case XmlEvent::Kind::End: break;
        }
    }
    return ExtractStatus::Ok;
}

namespace {

bool rtfSkippedDestination(std::string_view word) {
    static const char* const kSkipped[] = {"fonttbl", "colortbl", "stylesheet", "pict", "themedata", "colorschememapping",
                                           "datastore", "latentstyles", "listtable", "listoverridetable", "rsidtbl",
                                           "xmlnstbl", "generator", "fldinst", "objdata", "info", "header", "headerl", "headerr",
                                           "headerf", "footer", "footerl", "footerr", "footerf", "private", "mmathPr", "pgdsctbl",
                                           "revtbl", "filetbl", "wgrffmtfilter", "xmlopen", "bkmkstart", "bkmkend", "shpinst"};
    for (const char* skipped : kSkipped) {
        if (word == skipped) return true;
    }
    return false;
}

}  // namespace

ExtractStatus extractRtf(const ByteSource& source, Context& ctx) {
    std::string rtf;
    if (source.size() > ctx.limits.maxPlainTextFileBytes) return ExtractStatus::TooLarge;
    if (!readAll(source, rtf, ctx.limits.maxPlainTextFileBytes)) return ExtractStatus::Unreadable;
    if (rtf.compare(0, 5, "{\\rtf") != 0) { ctx.result.detail = "not an RTF document"; return ExtractStatus::Corrupt; }

    struct State { bool skip; int unicodeSkip; };
    std::vector<State> stack;
    State state{false, 1};
    int pendingSkip = 0;  // characters to drop after a \uN
    std::size_t i = 0;
    while (i < rtf.size() && !ctx.sink.full()) {
        if (ctx.budget.expired()) break;
        const char c = rtf[i];
        if (c == '{') {
            stack.push_back(state);
            ++i;
            // An ignorable destination "{\*\word": skipped as a whole.
            if (i + 1 < rtf.size() && rtf[i] == '\\' && rtf[i + 1] == '*') state.skip = true;
            else if (i < rtf.size() && rtf[i] == '\\') {
                std::size_t j = i + 1;
                while (j < rtf.size() && std::isalpha(static_cast<unsigned char>(rtf[j]))) ++j;
                if (rtfSkippedDestination(std::string_view(rtf).substr(i + 1, j - i - 1))) state.skip = true;
            }
            continue;
        }
        if (c == '}') {
            if (!stack.empty()) { state = stack.back(); stack.pop_back(); }
            ++i;
            continue;
        }
        if (c == '\\') {
            ++i;
            if (i >= rtf.size()) break;
            const char d = rtf[i];
            if (d == '\\' || d == '{' || d == '}') {
                if (!state.skip && pendingSkip == 0) ctx.sink.append(d);
                else if (pendingSkip > 0) --pendingSkip;
                ++i;
            } else if (d == '\'') {
                if (i + 2 < rtf.size() + 0 && std::isxdigit(static_cast<unsigned char>(rtf[i + 1])) && std::isxdigit(static_cast<unsigned char>(rtf[i + 2]))) {
                    const unsigned value = static_cast<unsigned>(std::strtoul(rtf.substr(i + 1, 2).c_str(), nullptr, 16));
                    if (pendingSkip > 0) --pendingSkip;
                    else if (!state.skip) {
                        std::string byte(1, static_cast<char>(value));
                        ctx.sink.append(latin1ToUtf8(byte));
                    }
                    i += 3;
                } else ++i;
            } else if (d == '~') { if (!state.skip) ctx.sink.space(); ++i; }
            else if (d == '-' || d == '_') { ++i; }
            else if (std::isalpha(static_cast<unsigned char>(d))) {
                std::size_t j = i;
                while (j < rtf.size() && std::isalpha(static_cast<unsigned char>(rtf[j]))) ++j;
                const std::string word = rtf.substr(i, j - i);
                bool negative = false;
                if (j < rtf.size() && rtf[j] == '-') { negative = true; ++j; }
                std::size_t digitsStart = j;
                while (j < rtf.size() && std::isdigit(static_cast<unsigned char>(rtf[j]))) ++j;
                long param = 0;
                const bool hasParam = j > digitsStart;
                if (hasParam) param = std::strtol(rtf.substr(digitsStart, j - digitsStart).c_str(), nullptr, 10);
                if (negative) param = -param;
                if (j < rtf.size() && rtf[j] == ' ') ++j;  // the delimiter space belongs to the control word
                i = j;
                if (word == "bin" && hasParam && param > 0) { i += static_cast<std::size_t>(param); }
                else if (word == "uc") state.unicodeSkip = static_cast<int>(param);
                else if (word == "u" && hasParam) {
                    if (!state.skip) {
                        std::uint32_t cp = static_cast<std::uint32_t>(param < 0 ? param + 65536 : param);
                        appendUtf8(ctx.sink.text(), cp);  // bypasses the cap check; bounded by the loop condition
                    }
                    pendingSkip = state.unicodeSkip;
                } else if (!state.skip) {
                    if (word == "par" || word == "line" || word == "sect" || word == "page") ctx.sink.newline();
                    else if (word == "tab" || word == "cell") ctx.sink.space();
                    else if (word == "row") ctx.sink.newline();
                    else if (word == "emdash") appendUtf8(ctx.sink.text(), 0x2014);
                    else if (word == "endash") appendUtf8(ctx.sink.text(), 0x2013);
                    else if (word == "bullet") appendUtf8(ctx.sink.text(), 0x2022);
                    else if (word == "lquote") appendUtf8(ctx.sink.text(), 0x2018);
                    else if (word == "rquote") appendUtf8(ctx.sink.text(), 0x2019);
                    else if (word == "ldblquote") appendUtf8(ctx.sink.text(), 0x201C);
                    else if (word == "rdblquote") appendUtf8(ctx.sink.text(), 0x201D);
                }
            } else {
                ++i;  // an unknown control symbol
            }
            continue;
        }
        if (c == '\r' || c == '\n') { ++i; continue; }
        if (pendingSkip > 0) { --pendingSkip; ++i; continue; }
        if (state.skip) { ++i; continue; }
        // A run of ordinary characters (Latin-1 unless already UTF-8; RTF proper is 7-bit).
        std::size_t j = i;
        while (j < rtf.size() && rtf[j] != '\\' && rtf[j] != '{' && rtf[j] != '}' && rtf[j] != '\r' && rtf[j] != '\n') ++j;
        const std::string_view run = std::string_view(rtf).substr(i, j - i);
        ctx.sink.append(isValidUtf8(run) ? std::string(run) : latin1ToUtf8(run));
        i = j;
    }
    return ExtractStatus::Ok;
}

// ----------------------------------------------------------------------------------------------- e-mail

namespace {

int base64Value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

std::string decodeBase64(std::string_view in) {
    std::string out;
    out.reserve(in.size() * 3 / 4);
    unsigned buffer = 0;
    int bits = 0;
    for (const char ch : in) {
        const int v = base64Value(static_cast<unsigned char>(ch));
        if (v < 0) continue;
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<char>((buffer >> bits) & 0xFF)); }
    }
    return out;
}

std::string decodeQuotedPrintable(std::string_view in, bool header) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '=' && i + 2 < in.size() + 0 && std::isxdigit(static_cast<unsigned char>(in[i + 1])) && std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out.push_back(static_cast<char>(std::strtoul(std::string(in.substr(i + 1, 2)).c_str(), nullptr, 16)));
            i += 2;
        } else if (c == '=' && i + 1 < in.size() && (in[i + 1] == '\n' || in[i + 1] == '\r')) {  // soft line break
            i += (in[i + 1] == '\r' && i + 2 < in.size() && in[i + 2] == '\n') ? 2 : 1;
        } else if (header && c == '_') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string toUtf8(std::string bytes, std::string_view charset) {
    std::string lowered(charset);
    for (char& c : lowered) c = lowerChar(c);
    if (lowered == "utf-16" || lowered == "utf-16le") return utf16ToUtf8(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), false);
    if (lowered == "utf-16be") return utf16ToUtf8(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), true);
    return isValidUtf8(bytes) ? bytes : latin1ToUtf8(bytes);
}

// RFC 2047 "=?charset?B?...?=" words in a header value.
std::string decodeHeaderWords(const std::string& value) {
    std::string out;
    std::size_t i = 0;
    while (i < value.size()) {
        const std::size_t start = value.find("=?", i);
        if (start == std::string::npos) { out.append(value, i, std::string::npos); break; }
        const std::size_t q1 = value.find('?', start + 2);
        const std::size_t q2 = q1 == std::string::npos ? q1 : value.find('?', q1 + 1);
        const std::size_t end = q2 == std::string::npos ? q2 : value.find("?=", q2 + 1);
        if (end == std::string::npos || q2 != q1 + 2) { out.append(value, i, start + 2 - i); i = start + 2; continue; }
        out.append(value, i, start - i);
        const std::string charset = value.substr(start + 2, q1 - start - 2);
        const char encoding = static_cast<char>(std::toupper(static_cast<unsigned char>(value[q1 + 1])));
        const std::string_view payload = std::string_view(value).substr(q2 + 1, end - q2 - 1);
        out += toUtf8(encoding == 'B' ? decodeBase64(payload) : decodeQuotedPrintable(payload, true), charset);
        i = end + 2;
    }
    return out;
}

struct Header { std::string name, value; };

std::vector<Header> parseHeaders(std::string_view block) {
    std::vector<Header> headers;
    std::size_t pos = 0;
    while (pos < block.size()) {
        std::size_t eol = block.find('\n', pos);
        if (eol == std::string_view::npos) eol = block.size();
        std::string_view line = block.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        pos = eol + 1;
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            if (!headers.empty()) { headers.back().value += ' '; headers.back().value += std::string(line.substr(line.find_first_not_of(" \t") == std::string_view::npos ? line.size() : line.find_first_not_of(" \t"))); }
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        Header h;
        h.name.assign(line.substr(0, colon));
        for (char& c : h.name) c = lowerChar(c);
        const std::size_t v = line.find_first_not_of(" \t", colon + 1);
        h.value.assign(v == std::string_view::npos ? std::string_view() : line.substr(v));
        headers.push_back(std::move(h));
    }
    return headers;
}

std::string headerValue(const std::vector<Header>& headers, const char* name) {
    for (const Header& h : headers) {
        if (h.name == name) return h.value;
    }
    return {};
}

// Parameter of a structured header ("boundary", "charset", "filename", "name").
std::string headerParam(const std::string& value, const char* param) {
    std::string lowered = value;
    for (char& c : lowered) c = lowerChar(c);
    const std::string key = std::string(param) + "=";
    std::size_t at = 0;
    while ((at = lowered.find(key, at)) != std::string::npos) {
        if (at == 0 || lowered[at - 1] == ';' || lowered[at - 1] == ' ' || lowered[at - 1] == '\t') break;
        at += key.size();
    }
    if (at == std::string::npos) return {};
    at += key.size();
    if (at < value.size() && value[at] == '"') {
        const std::size_t end = value.find('"', at + 1);
        return value.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
    }
    const std::size_t end = value.find_first_of("; \t", at);
    return value.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

void extractMimePart(std::string_view part, Context& ctx, int depth, bool topLevel) {
    if (depth > 8 || ctx.sink.full() || ctx.budget.expired()) return;
    std::size_t split = part.find("\r\n\r\n");
    std::size_t gap = 4;
    const std::size_t split2 = part.find("\n\n");
    if (split == std::string_view::npos || (split2 != std::string_view::npos && split2 < split)) { split = split2; gap = 2; }
    std::string_view headerBlock = split == std::string_view::npos ? part : part.substr(0, split);
    std::string_view body = split == std::string_view::npos ? std::string_view() : part.substr(split + gap);
    const std::vector<Header> headers = parseHeaders(headerBlock);

    if (topLevel) {
        for (const char* name : {"subject", "from", "to", "cc", "date"}) {
            const std::string value = headerValue(headers, name);
            if (value.empty()) continue;
            ctx.sink.append(std::string(name) + ": " + decodeHeaderWords(value));
            ctx.sink.newline();
        }
        ctx.sink.newline();
    }

    std::string contentType = headerValue(headers, "content-type");
    std::string typeLower = contentType;
    for (char& c : typeLower) c = lowerChar(c);
    if (typeLower.empty()) typeLower = "text/plain";
    std::string transfer = headerValue(headers, "content-transfer-encoding");
    for (char& c : transfer) c = lowerChar(c);

    if (typeLower.rfind("multipart/", 0) == 0) {
        const std::string boundary = headerParam(contentType, "boundary");
        if (boundary.empty()) return;
        const std::string delimiter = "--" + boundary;
        std::size_t pos = body.find(delimiter);
        while (pos != std::string_view::npos && !ctx.sink.full()) {
            std::size_t start = body.find('\n', pos);
            if (start == std::string_view::npos) break;
            ++start;
            const std::size_t next = body.find(delimiter, start);
            std::string_view sub = body.substr(start, next == std::string_view::npos ? std::string_view::npos : next - start);
            extractMimePart(sub, ctx, depth + 1, false);
            if (next != std::string_view::npos && body.compare(next + delimiter.size(), 2, "--") == 0) break;
            pos = next;
        }
        return;
    }
    const std::string disposition = headerValue(headers, "content-disposition");
    const std::string filename = decodeHeaderWords(headerParam(disposition, "filename").empty() ? headerParam(contentType, "name") : headerParam(disposition, "filename"));
    if (!filename.empty()) {
        ctx.sink.append("Attachment: " + filename);
        ctx.sink.newline();
    }
    const bool isText = typeLower.rfind("text/plain", 0) == 0 || typeLower.rfind("text/html", 0) == 0;
    const bool isAttachment = disposition.find("attachment") != std::string::npos;
    if (typeLower.rfind("message/rfc822", 0) == 0) {
        extractMimePart(body, ctx, depth + 1, true);
        return;
    }
    if (!isText || (isAttachment && !filename.empty() && typeLower.rfind("text/", 0) != 0)) return;

    std::string decoded;
    if (transfer == "base64") decoded = decodeBase64(body);
    else if (transfer == "quoted-printable") decoded = decodeQuotedPrintable(body, false);
    else decoded.assign(body);
    const std::string text = toUtf8(std::move(decoded), headerParam(contentType, "charset"));
    if (typeLower.rfind("text/html", 0) == 0) {
        htmlToText(text, ctx.sink, ctx.budget);
    } else {
        ctx.sink.append(text);
    }
    ctx.sink.newline();
}

}  // namespace

ExtractStatus extractEml(const ByteSource& source, Context& ctx) {
    std::string raw;
    if (source.size() > ctx.limits.maxPlainTextFileBytes) return ExtractStatus::TooLarge;
    if (!readAll(source, raw, ctx.limits.maxPlainTextFileBytes)) return ExtractStatus::Unreadable;
    if (raw.find(':') == std::string::npos) { ctx.result.detail = "not an e-mail message"; return ExtractStatus::Corrupt; }
    extractMimePart(raw, ctx, 0, true);
    return ExtractStatus::Ok;
}

}  // namespace flux::extract_detail
