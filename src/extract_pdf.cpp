// PDF text extraction, written for Flux (no PDF library): cross-reference tables and streams, object
// streams, page tree, content-stream text operators, simple and composite fonts with ToUnicode maps.
//
// Nothing in a PDF is executed: JavaScript, actions, forms, launch entries and embedded files are never
// looked at. Only page content streams are interpreted, and only for their text-showing operators.
//
// Memory is bounded: the file is read through small windows, objects are fetched on demand, and only one
// page's decoded content (each stream capped by ExtractLimits::maxStreamBytes) is in memory at a time.
#include "extract_internal.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <unordered_map>

namespace flux::extract_detail {
namespace {

// ===================================================================================== PDF objects

struct Obj {
    enum class Type { Null, Bool, Int, Real, Name, String, Array, Dict, Ref, Op } type = Type::Null;
    double num = 0;
    std::int64_t integer = 0;  // Int value, Ref object number
    int gen = 0;
    std::string str;                // Name, String (raw bytes), Op keyword
    std::vector<Obj> items;         // Array elements / Dict values
    std::vector<std::string> keys;  // Dict keys, parallel to `items`
    std::int64_t streamPos = -1;    // absolute offset of the stream data of a stream object
    std::int64_t number = 0;        // object number this was loaded as (0 for direct objects)

    bool isNum() const { return type == Type::Int || type == Type::Real; }
    double asNumber() const { return type == Type::Int ? static_cast<double>(integer) : num; }
    bool isName(const char* n) const { return type == Type::Name && str == n; }
    bool isStream() const { return streamPos >= 0; }
    const Obj* get(const char* key) const {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) return &items[i];
        }
        return nullptr;
    }
};

bool isWhite(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0'; }
bool isDelim(char c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }

class Parser {
public:
    explicit Parser(std::string_view data, std::size_t pos = 0) : d_(data), pos_(pos) {}
    std::size_t pos() const { return pos_; }
    void setPos(std::size_t p) { pos_ = p; }
    bool atEnd() { skipWhite(); return pos_ >= d_.size(); }
    std::string_view data() const { return d_; }

    void skipWhite() {
        while (pos_ < d_.size()) {
            if (isWhite(d_[pos_])) ++pos_;
            else if (d_[pos_] == '%') { while (pos_ < d_.size() && d_[pos_] != '\n' && d_[pos_] != '\r') ++pos_; }
            else break;
        }
    }

    // Allocation-free fast paths for content streams: a plain number, or an operator word.
    bool fastNumber(double& value) {
        skipWhite();
        if (pos_ >= d_.size()) return false;
        std::size_t i = pos_;
        const char c = d_[i];
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.')) return false;
        bool negative = false;
        if (c == '-' || c == '+') { negative = c == '-'; ++i; }
        double result = 0;
        bool any = false;
        while (i < d_.size() && std::isdigit(static_cast<unsigned char>(d_[i]))) { result = result * 10 + (d_[i] - '0'); ++i; any = true; }
        if (i < d_.size() && d_[i] == '.') {
            ++i;
            double scale = 0.1;
            while (i < d_.size() && std::isdigit(static_cast<unsigned char>(d_[i]))) { result += (d_[i] - '0') * scale; scale *= 0.1; ++i; any = true; }
        }
        if (!any || (i < d_.size() && !isWhite(d_[i]) && !isDelim(d_[i]))) return false;  // odd token: use the general parser
        value = negative ? -result : result;
        pos_ = i;
        return true;
    }
    bool fastWord(std::string_view& word) {
        skipWhite();
        if (pos_ >= d_.size()) return false;
        const char c = d_[pos_];
        if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '\'' || c == '"')) return false;
        const std::size_t start = pos_;
        std::size_t i = pos_;
        while (i < d_.size() && !isWhite(d_[i]) && !isDelim(d_[i])) ++i;
        word = d_.substr(start, i - start);
        pos_ = i;
        return true;
    }

    // Parses one object (or, in content streams, one operator as Type::Op). False at the end or on error.
    bool parse(Obj& out, bool allowRefs, int depth = 0) {
        out = Obj{};
        if (depth > 48) return false;
        skipWhite();
        if (pos_ >= d_.size()) return false;
        const char c = d_[pos_];
        if (c == '/') { ++pos_; out.type = Obj::Type::Name; out.str = readName(); return true; }
        if (c == '(') return readLiteralString(out);
        if (c == '<') {
            if (pos_ + 1 < d_.size() && d_[pos_ + 1] == '<') return readDict(out, allowRefs, depth);
            return readHexString(out);
        }
        if (c == '[') {
            ++pos_;
            out.type = Obj::Type::Array;
            for (;;) {
                skipWhite();
                if (pos_ >= d_.size()) return false;
                if (d_[pos_] == ']') { ++pos_; return true; }
                Obj item;
                if (!parse(item, allowRefs, depth + 1)) return false;
                if (item.type == Obj::Type::Op && item.str.size() == 1 && (item.str[0] == '>' || item.str[0] == ')' || item.str[0] == '}')) continue;
                out.items.push_back(std::move(item));
            }
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.') return readNumber(out, allowRefs);
        if (c == ']' || c == ')' || c == '>' || c == '}' || c == '{') { ++pos_; out.type = Obj::Type::Op; out.str = std::string(1, c); return true; }
        const std::size_t start = pos_;
        while (pos_ < d_.size() && !isWhite(d_[pos_]) && !isDelim(d_[pos_])) ++pos_;
        if (pos_ == start) { ++pos_; out.type = Obj::Type::Op; out.str = "?"; return true; }
        const std::string_view word = d_.substr(start, pos_ - start);
        if (word == "true" || word == "false") { out.type = Obj::Type::Bool; out.num = word == "true" ? 1 : 0; return true; }
        if (word == "null") { out.type = Obj::Type::Null; return true; }
        out.type = Obj::Type::Op;
        out.str.assign(word);
        return true;
    }

private:
    std::string readName() {
        std::string name;
        while (pos_ < d_.size() && !isWhite(d_[pos_]) && !isDelim(d_[pos_])) {
            if (d_[pos_] == '#' && pos_ + 2 < d_.size() + 0 && std::isxdigit(static_cast<unsigned char>(d_[pos_ + 1])) && std::isxdigit(static_cast<unsigned char>(d_[pos_ + 2]))) {
                name.push_back(static_cast<char>(std::strtoul(std::string(d_.substr(pos_ + 1, 2)).c_str(), nullptr, 16)));
                pos_ += 3;
            } else {
                name.push_back(d_[pos_++]);
            }
        }
        return name;
    }

    bool readLiteralString(Obj& out) {
        ++pos_;
        out.type = Obj::Type::String;
        int depth = 1;
        while (pos_ < d_.size()) {
            const char c = d_[pos_++];
            if (c == '\\') {
                if (pos_ >= d_.size()) break;
                const char e = d_[pos_++];
                switch (e) {
                    case 'n': out.str.push_back('\n'); break;
                    case 'r': out.str.push_back('\r'); break;
                    case 't': out.str.push_back('\t'); break;
                    case 'b': out.str.push_back('\b'); break;
                    case 'f': out.str.push_back('\f'); break;
                    case '\r': if (pos_ < d_.size() && d_[pos_] == '\n') ++pos_; break;
                    case '\n': break;
                    default:
                        if (e >= '0' && e <= '7') {
                            int value = e - '0';
                            for (int k = 0; k < 2 && pos_ < d_.size() && d_[pos_] >= '0' && d_[pos_] <= '7'; ++k) value = value * 8 + (d_[pos_++] - '0');
                            out.str.push_back(static_cast<char>(value & 0xFF));
                        } else {
                            out.str.push_back(e);
                        }
                }
            } else if (c == '(') { ++depth; out.str.push_back(c); }
            else if (c == ')') { if (--depth == 0) return true; out.str.push_back(c); }
            else out.str.push_back(c);
        }
        return true;  // unterminated: keep what there is
    }

    bool readHexString(Obj& out) {
        ++pos_;
        out.type = Obj::Type::String;
        int high = -1;
        while (pos_ < d_.size()) {
            const char c = d_[pos_++];
            if (c == '>') break;
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else continue;
            if (high < 0) high = v;
            else { out.str.push_back(static_cast<char>(high * 16 + v)); high = -1; }
        }
        if (high >= 0) out.str.push_back(static_cast<char>(high * 16));
        return true;
    }

    bool readDict(Obj& out, bool allowRefs, int depth) {
        pos_ += 2;
        out.type = Obj::Type::Dict;
        for (;;) {
            skipWhite();
            if (pos_ >= d_.size()) return false;
            if (d_[pos_] == '>') { pos_ += (pos_ + 1 < d_.size() && d_[pos_ + 1] == '>') ? 2 : 1; return true; }
            Obj key;
            if (!parse(key, allowRefs, depth + 1)) return false;
            if (key.type != Obj::Type::Name) continue;  // damaged: skip
            Obj value;
            skipWhite();
            if (pos_ < d_.size() && d_[pos_] == '>') { out.keys.push_back(key.str); out.items.push_back(Obj{}); continue; }
            if (!parse(value, allowRefs, depth + 1)) return false;
            out.keys.push_back(std::move(key.str));
            out.items.push_back(std::move(value));
        }
    }

    bool readNumber(Obj& out, bool allowRefs) {
        const std::size_t start = pos_;
        bool real = false;
        if (d_[pos_] == '+' || d_[pos_] == '-') ++pos_;
        while (pos_ < d_.size() && (std::isdigit(static_cast<unsigned char>(d_[pos_])) || d_[pos_] == '.' || d_[pos_] == '-')) {
            if (d_[pos_] == '.') real = true;
            ++pos_;
        }
        const std::string token(d_.substr(start, pos_ - start));
        if (token.empty() || token == "+" || token == "-" || token == ".") { out.type = Obj::Type::Op; out.str = token.empty() ? "?" : token; if (token.empty()) ++pos_; return true; }
        if (real) {
            out.type = Obj::Type::Real;
            out.num = std::strtod(token.c_str(), nullptr);
            return true;
        }
        out.type = Obj::Type::Int;
        out.integer = std::strtoll(token.c_str(), nullptr, 10);
        out.num = static_cast<double>(out.integer);
        if (allowRefs && out.integer >= 0) {
            // "N G R" is an indirect reference.
            std::size_t p = pos_;
            while (p < d_.size() && isWhite(d_[p])) ++p;
            const std::size_t genStart = p;
            while (p < d_.size() && std::isdigit(static_cast<unsigned char>(d_[p]))) ++p;
            if (p > genStart && p - genStart <= 5) {
                const std::size_t genEnd = p;
                while (p < d_.size() && isWhite(d_[p])) ++p;
                if (p < d_.size() && d_[p] == 'R' && (p + 1 >= d_.size() || isWhite(d_[p + 1]) || isDelim(d_[p + 1]))) {
                    out.type = Obj::Type::Ref;
                    out.gen = std::atoi(std::string(d_.substr(genStart, genEnd - genStart)).c_str());
                    pos_ = p + 1;
                }
            }
        }
        return true;
    }

    std::string_view d_;
    std::size_t pos_;
};

// ===================================================================================== stream filters

void applyPngPredictor(std::string& data, int columns, int colors, int bits) {
    const int bytesPerPixel = std::max(1, (colors * bits + 7) / 8);
    const std::size_t rowBytes = static_cast<std::size_t>((static_cast<long long>(columns) * colors * bits + 7) / 8);
    if (rowBytes == 0) return;
    const std::size_t stride = rowBytes + 1;
    const std::size_t rows = (data.size() + stride - 1) / stride;
    std::string out;
    out.resize(rows * rowBytes);
    std::string zeroRow(rowBytes, '\0');
    std::size_t written = 0;
    for (std::size_t r = 0; r < rows; ++r) {
        const std::size_t base = r * stride;
        if (base >= data.size()) break;
        const unsigned char type = static_cast<unsigned char>(data[base]);
        const std::size_t avail = std::min(rowBytes, data.size() - base - 1);
        unsigned char* cur = reinterpret_cast<unsigned char*>(&out[r * rowBytes]);
        const unsigned char* prev = r == 0 ? reinterpret_cast<const unsigned char*>(zeroRow.data()) : reinterpret_cast<const unsigned char*>(&out[(r - 1) * rowBytes]);
        const unsigned char* in = reinterpret_cast<const unsigned char*>(&data[base + 1]);
        for (std::size_t i = 0; i < avail; ++i) {
            const int a = i >= static_cast<std::size_t>(bytesPerPixel) ? cur[i - bytesPerPixel] : 0;
            const int b = prev[i];
            const int c = i >= static_cast<std::size_t>(bytesPerPixel) ? prev[i - bytesPerPixel] : 0;
            int value = in[i];
            switch (type) {
                case 1: value += a; break;
                case 2: value += b; break;
                case 3: value += (a + b) / 2; break;
                case 4: {
                    const int p = a + b - c;
                    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    value += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
                default: break;
            }
            cur[i] = static_cast<unsigned char>(value);
        }
        written = r * rowBytes + avail;
    }
    out.resize(written);
    data.swap(out);
}

std::string lzwDecode(const std::string& in, int earlyChange, std::size_t maxOut) {
    std::string out;
    std::vector<std::pair<int, unsigned char>> table;  // prefix code, last byte; code = 258 + index
    table.reserve(4096);
    auto expand = [&](int code, std::string& dst) {
        std::string tmp;
        while (code >= 258) {
            tmp.push_back(static_cast<char>(table[static_cast<std::size_t>(code - 258)].second));
            code = table[static_cast<std::size_t>(code - 258)].first;
        }
        tmp.push_back(static_cast<char>(code));
        dst.append(tmp.rbegin(), tmp.rend());
    };
    unsigned buffer = 0;
    int bits = 0, width = 9, previous = -1;
    for (const char ch : in) {
        buffer = (buffer << 8) | static_cast<unsigned char>(ch);
        bits += 8;
        while (bits >= width) {
            const int code = static_cast<int>((buffer >> (bits - width)) & ((1u << width) - 1));
            bits -= width;
            if (code == 256) { table.clear(); width = 9; previous = -1; continue; }
            if (code == 257) return out;
            std::string entry;
            if (code < 256) entry.push_back(static_cast<char>(code));
            else if (code - 258 < static_cast<int>(table.size())) expand(code, entry);
            else if (code - 258 == static_cast<int>(table.size()) && previous >= 0) { expand(previous, entry); entry.push_back(entry[0]); }
            else return out;
            if (previous >= 0 && table.size() < 4096 - 258) table.push_back({previous, static_cast<unsigned char>(entry[0])});
            out += entry;
            if (out.size() > maxOut) return out;
            previous = code;
            const int next = static_cast<int>(table.size()) + 258 + earlyChange;
            width = next >= 2048 ? 12 : next >= 1024 ? 11 : next >= 512 ? 10 : 9;
        }
    }
    return out;
}

std::string ascii85Decode(const std::string& in) {
    std::string out;
    unsigned long long value = 0;
    int count = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (isWhite(c)) continue;
        if (c == '~') break;
        if (c == 'z' && count == 0) { out.append(4, '\0'); continue; }
        if (c < '!' || c > 'u') continue;
        value = value * 85 + static_cast<unsigned>(c - '!');
        if (++count == 5) {
            for (int k = 3; k >= 0; --k) out.push_back(static_cast<char>((value >> (8 * k)) & 0xFF));
            value = 0; count = 0;
        }
    }
    if (count > 1) {
        for (int k = count; k < 5; ++k) value = value * 85 + 84;
        for (int k = 3; k > 4 - count; --k) out.push_back(static_cast<char>((value >> (8 * k)) & 0xFF));
    }
    return out;
}

std::string asciiHexDecode(const std::string& in) {
    std::string out;
    int high = -1;
    for (const char c : in) {
        if (c == '>') break;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;
        if (high < 0) high = v;
        else { out.push_back(static_cast<char>(high * 16 + v)); high = -1; }
    }
    if (high >= 0) out.push_back(static_cast<char>(high * 16));
    return out;
}

std::string runLengthDecode(const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size();) {
        const unsigned char n = static_cast<unsigned char>(in[i++]);
        if (n == 128) break;
        if (n < 128) { const std::size_t len = n + 1u; out.append(in, i, len); i += len; }
        else if (i < in.size()) { out.append(257u - n, in[i++]); }
    }
    return out;
}

// ======================================================================================= fonts / CMaps

std::string utf16beToUtf8(const std::string& bytes) {
    return utf16ToUtf8(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), true);
}

struct GlyphName { const char* name; std::uint16_t cp; };
const GlyphName kGlyphs[] = {
    {"space", 32}, {"exclam", 33}, {"quotedbl", 34}, {"numbersign", 35}, {"dollar", 36}, {"percent", 37}, {"ampersand", 38},
    {"quotesingle", 39}, {"parenleft", 40}, {"parenright", 41}, {"asterisk", 42}, {"plus", 43}, {"comma", 44}, {"hyphen", 45},
    {"period", 46}, {"slash", 47}, {"zero", 48}, {"one", 49}, {"two", 50}, {"three", 51}, {"four", 52}, {"five", 53}, {"six", 54},
    {"seven", 55}, {"eight", 56}, {"nine", 57}, {"colon", 58}, {"semicolon", 59}, {"less", 60}, {"equal", 61}, {"greater", 62},
    {"question", 63}, {"at", 64}, {"bracketleft", 91}, {"backslash", 92}, {"bracketright", 93}, {"asciicircum", 94},
    {"underscore", 95}, {"grave", 96}, {"braceleft", 123}, {"bar", 124}, {"braceright", 125}, {"asciitilde", 126},
    {"quoteright", 0x2019}, {"quoteleft", 0x2018}, {"quotedblleft", 0x201C}, {"quotedblright", 0x201D}, {"quotesinglbase", 0x201A},
    {"quotedblbase", 0x201E}, {"endash", 0x2013}, {"emdash", 0x2014}, {"bullet", 0x2022}, {"ellipsis", 0x2026}, {"dagger", 0x2020},
    {"daggerdbl", 0x2021}, {"perthousand", 0x2030}, {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A}, {"trademark", 0x2122},
    {"Euro", 0x20AC}, {"fi", 0xFB01}, {"fl", 0xFB02}, {"ff", 0xFB00}, {"ffi", 0xFB03}, {"ffl", 0xFB04}, {"minus", 0x2212},
    {"nbspace", 0xA0}, {"nonbreakingspace", 0xA0}, {"sfthyphen", 0xAD}, {"exclamdown", 0xA1}, {"cent", 0xA2}, {"sterling", 0xA3},
    {"currency", 0xA4}, {"yen", 0xA5}, {"brokenbar", 0xA6}, {"section", 0xA7}, {"dieresis", 0xA8}, {"copyright", 0xA9},
    {"ordfeminine", 0xAA}, {"guillemotleft", 0xAB}, {"logicalnot", 0xAC}, {"registered", 0xAE}, {"macron", 0xAF}, {"degree", 0xB0},
    {"plusminus", 0xB1}, {"twosuperior", 0xB2}, {"threesuperior", 0xB3}, {"acute", 0xB4}, {"mu", 0xB5}, {"paragraph", 0xB6},
    {"periodcentered", 0xB7}, {"cedilla", 0xB8}, {"onesuperior", 0xB9}, {"ordmasculine", 0xBA}, {"guillemotright", 0xBB},
    {"onequarter", 0xBC}, {"onehalf", 0xBD}, {"threequarters", 0xBE}, {"questiondown", 0xBF}, {"Agrave", 0xC0}, {"Aacute", 0xC1},
    {"Acircumflex", 0xC2}, {"Atilde", 0xC3}, {"Adieresis", 0xC4}, {"Aring", 0xC5}, {"AE", 0xC6}, {"Ccedilla", 0xC7},
    {"Egrave", 0xC8}, {"Eacute", 0xC9}, {"Ecircumflex", 0xCA}, {"Edieresis", 0xCB}, {"Igrave", 0xCC}, {"Iacute", 0xCD},
    {"Icircumflex", 0xCE}, {"Idieresis", 0xCF}, {"Eth", 0xD0}, {"Ntilde", 0xD1}, {"Ograve", 0xD2}, {"Oacute", 0xD3},
    {"Ocircumflex", 0xD4}, {"Otilde", 0xD5}, {"Odieresis", 0xD6}, {"multiply", 0xD7}, {"Oslash", 0xD8}, {"Ugrave", 0xD9},
    {"Uacute", 0xDA}, {"Ucircumflex", 0xDB}, {"Udieresis", 0xDC}, {"Yacute", 0xDD}, {"Thorn", 0xDE}, {"germandbls", 0xDF},
    {"agrave", 0xE0}, {"aacute", 0xE1}, {"acircumflex", 0xE2}, {"atilde", 0xE3}, {"adieresis", 0xE4}, {"aring", 0xE5}, {"ae", 0xE6},
    {"ccedilla", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9}, {"ecircumflex", 0xEA}, {"edieresis", 0xEB}, {"igrave", 0xEC},
    {"iacute", 0xED}, {"icircumflex", 0xEE}, {"idieresis", 0xEF}, {"eth", 0xF0}, {"ntilde", 0xF1}, {"ograve", 0xF2},
    {"oacute", 0xF3}, {"ocircumflex", 0xF4}, {"otilde", 0xF5}, {"odieresis", 0xF6}, {"divide", 0xF7}, {"oslash", 0xF8},
    {"ugrave", 0xF9}, {"uacute", 0xFA}, {"ucircumflex", 0xFB}, {"udieresis", 0xFC}, {"yacute", 0xFD}, {"thorn", 0xFE},
    {"ydieresis", 0xFF}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Zcaron", 0x17D}, {"zcaron", 0x17E}, {"OE", 0x152}, {"oe", 0x153},
    {"Ydieresis", 0x178}, {"florin", 0x192}, {"circumflex", 0x2C6}, {"tilde", 0x2DC}, {"dotlessi", 0x131}, {"Lslash", 0x141},
    {"lslash", 0x142}, {"fraction", 0x2044}, {"summation", 0x2211}, {"infinity", 0x221E}, {"notequal", 0x2260},
    {"lessequal", 0x2264}, {"greaterequal", 0x2265}, {"radical", 0x221A}, {"partialdiff", 0x2202}, {"integral", 0x222B},
    {"approxequal", 0x2248}, {"Omega", 0x3A9}, {"pi", 0x3C0}, {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4},
    {"lozenge", 0x25CA}, {"arrowright", 0x2192}, {"arrowleft", 0x2190}, {"checkmark", 0x2713}, {"hungarumlaut", 0x2DD},
    {"ogonek", 0x2DB}, {"caron", 0x2C7}, {"breve", 0x2D8}, {"dotaccent", 0x2D9}, {"ring", 0x2DA},
};

std::uint32_t glyphNameToUnicode(const std::string& fullName, std::string& multi) {
    multi.clear();
    std::string name = fullName.substr(0, fullName.find('.'));
    if (name.empty()) return 0;
    if (name.size() == 1) return static_cast<unsigned char>(name[0]);
    static const std::unordered_map<std::string, std::uint16_t> table = [] {
        std::unordered_map<std::string, std::uint16_t> t;
        for (const GlyphName& g : kGlyphs) t.emplace(g.name, g.cp);
        return t;
    }();
    const auto it = table.find(name);
    if (it != table.end()) return it->second;
    if (name.size() >= 7 && name.compare(0, 3, "uni") == 0 && name.find_first_not_of("0123456789ABCDEFabcdef", 3) == std::string::npos) {
        return static_cast<std::uint32_t>(std::strtoul(name.substr(3, 4).c_str(), nullptr, 16));
    }
    if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u' && name.find_first_not_of("0123456789ABCDEFabcdef", 1) == std::string::npos) {
        return static_cast<std::uint32_t>(std::strtoul(name.c_str() + 1, nullptr, 16));
    }
    if (name.find('_') != std::string::npos) {  // ligature components: f_i
        std::size_t start = 0;
        while (start <= name.size()) {
            std::size_t end = name.find('_', start);
            if (end == std::string::npos) end = name.size();
            std::string inner;
            const std::uint32_t cp = glyphNameToUnicode(name.substr(start, end - start), inner);
            if (cp != 0) appendUtf8(multi, cp);
            start = end + 1;
        }
        return multi.empty() ? 0 : 0xFFFFFFFFu;
    }
    return 0;
}

const std::uint16_t kWin1252High[32] = {0x20AC, 0x2022, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                        0x2039, 0x0152, 0x2022, 0x017D, 0x2022, 0x2022, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x2022, 0x017E, 0x0178};
const std::uint16_t kMacRomanHigh[128] = {
    0xC4, 0xC5, 0xC7, 0xC9, 0xD1, 0xD6, 0xDC, 0xE1, 0xE0, 0xE2, 0xE4, 0xE3, 0xE5, 0xE7, 0xE9, 0xE8,
    0xEA, 0xEB, 0xED, 0xEC, 0xEE, 0xEF, 0xF1, 0xF3, 0xF2, 0xF4, 0xF6, 0xF5, 0xFA, 0xF9, 0xFB, 0xFC,
    0x2020, 0xB0, 0xA2, 0xA3, 0xA7, 0x2022, 0xB6, 0xDF, 0xAE, 0xA9, 0x2122, 0xB4, 0xA8, 0x2260, 0xC6, 0xD8,
    0x221E, 0xB1, 0x2264, 0x2265, 0xA5, 0xB5, 0x2202, 0x2211, 0x220F, 0x3C0, 0x222B, 0xAA, 0xBA, 0x3A9, 0xE6, 0xF8,
    0xBF, 0xA1, 0xAC, 0x221A, 0x192, 0x2248, 0x2206, 0xAB, 0xBB, 0x2026, 0xA0, 0xC0, 0xC3, 0xD5, 0x152, 0x153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7, 0x25CA, 0xFF, 0x178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0xB7, 0x201A, 0x201E, 0x2030, 0xC2, 0xCA, 0xC1, 0xCB, 0xC8, 0xCD, 0xCE, 0xCF, 0xCC, 0xD3, 0xD4,
    0xF8FF, 0xD2, 0xDA, 0xDB, 0xD9, 0x131, 0x2C6, 0x2DC, 0xAF, 0x2D8, 0x2D9, 0x2DA, 0xB8, 0x2DD, 0x2DB, 0x2C7};

enum class BaseEncoding { Standard, WinAnsi, MacRoman };

void fillBaseEncoding(std::uint32_t (&table)[256], BaseEncoding base) {
    for (int c = 0; c < 256; ++c) table[c] = 0;
    for (int c = 0x20; c < 0x7F; ++c) table[c] = static_cast<std::uint32_t>(c);
    switch (base) {
        case BaseEncoding::WinAnsi:
            for (int c = 0x80; c < 0xA0; ++c) table[c] = kWin1252High[c - 0x80];
            for (int c = 0xA0; c < 0x100; ++c) table[c] = static_cast<std::uint32_t>(c);
            table[0x7F] = 0x2022;
            break;
        case BaseEncoding::MacRoman:
            for (int c = 0x80; c < 0x100; ++c) table[c] = kMacRomanHigh[c - 0x80];
            break;
        case BaseEncoding::Standard: {
            table[0x27] = 0x2019; table[0x60] = 0x2018;
            static const std::pair<int, std::uint32_t> upper[] = {
                {0xA1, 0xA1}, {0xA2, 0xA2}, {0xA3, 0xA3}, {0xA4, 0x2044}, {0xA5, 0xA5}, {0xA6, 0x192}, {0xA7, 0xA7}, {0xA8, 0xA4},
                {0xA9, 0x27}, {0xAA, 0x201C}, {0xAB, 0xAB}, {0xAC, 0x2039}, {0xAD, 0x203A}, {0xAE, 0xFB01}, {0xAF, 0xFB02},
                {0xB1, 0x2013}, {0xB2, 0x2020}, {0xB3, 0x2021}, {0xB4, 0xB7}, {0xB6, 0xB6}, {0xB7, 0x2022}, {0xB8, 0x201A},
                {0xB9, 0x201E}, {0xBA, 0x201D}, {0xBB, 0xBB}, {0xBC, 0x2026}, {0xBD, 0x2030}, {0xBF, 0xBF}, {0xC1, 0x60},
                {0xC2, 0xB4}, {0xC3, 0x2C6}, {0xC4, 0x2DC}, {0xC5, 0xAF}, {0xC6, 0x2D8}, {0xC7, 0x2D9}, {0xC8, 0xA8},
                {0xCA, 0x2DA}, {0xCB, 0xB8}, {0xCD, 0x2DD}, {0xCE, 0x2DB}, {0xCF, 0x2C7}, {0xD0, 0x2014}, {0xE1, 0xC6},
                {0xE3, 0xAA}, {0xE8, 0x141}, {0xE9, 0xD8}, {0xEA, 0x152}, {0xEB, 0xBA}, {0xF1, 0xE6}, {0xF5, 0x131},
                {0xF8, 0x142}, {0xF9, 0xF8}, {0xFA, 0x153}, {0xFB, 0xDF}};
            for (const auto& u : upper) table[u.first] = u.second;
            break;
        }
    }
}

struct Font {
    bool composite = false;
    bool hasToUnicode = false;
    bool unicodeCodes = false;  // composite font whose codes are UTF-16 (UCS2 CMaps)
    std::uint32_t encoding[256] = {};
    std::unordered_map<std::uint64_t, std::string> map;  // (length << 32 | code) -> UTF-8
    struct Range { std::uint32_t lo, hi, cp; std::uint8_t length; };
    std::vector<Range> ranges;
    struct Space { std::uint8_t length; std::uint32_t lo, hi; };
    std::vector<Space> codespaces;
    // widths in 1/1000 text units
    int firstChar = 0;
    std::vector<double> widths;
    double widthScale = 1.0;  // Type 3 fonts: widths are in glyph space; the FontMatrix turns them into 1/1000 em
    double missingWidth = 0;
    std::unordered_map<std::uint32_t, double> cidWidths;
    double defaultWidth = 1000;
    bool unmappable = false;  // composite font with no way to turn codes into Unicode
};

void appendLigatureSafe(std::string& out, const std::string& utf8) {
    // Ligature glyphs are split into letters so that "office" matches a query for "office".
    for (std::size_t i = 0; i < utf8.size();) {
        if (static_cast<unsigned char>(utf8[i]) == 0xEF && i + 2 < utf8.size() && static_cast<unsigned char>(utf8[i + 1]) == 0xAC) {
            const unsigned char third = static_cast<unsigned char>(utf8[i + 2]);
            const char* repl = nullptr;
            switch (third) {
                case 0x80: repl = "ff"; break; case 0x81: repl = "fi"; break; case 0x82: repl = "fl"; break;
                case 0x83: repl = "ffi"; break; case 0x84: repl = "ffl"; break; case 0x85: case 0x86: repl = "st"; break;
                default: break;
            }
            if (repl != nullptr) { out += repl; i += 3; continue; }
        }
        out.push_back(utf8[i++]);
    }
}

bool codeInSpace(const Font::Space& s, std::uint32_t code) {
    for (int b = 0; b < s.length; ++b) {
        const int shift = 8 * (s.length - 1 - b);
        const std::uint32_t c = (code >> shift) & 0xFF, lo = (s.lo >> shift) & 0xFF, hi = (s.hi >> shift) & 0xFF;
        if (c < lo || c > hi) return false;
    }
    return true;
}

void parseCMap(const std::string& text, Font& font) {
    Parser parser(text);
    Obj token;
    auto codeOf = [](const std::string& bytes) {
        std::uint32_t v = 0;
        for (unsigned char b : bytes) v = (v << 8) | b;
        return v;
    };
    std::vector<Obj> operands;
    int guard = 0;
    while (parser.parse(token, false) && ++guard < 4000000) {
        if (token.type != Obj::Type::Op) { operands.push_back(token); if (operands.size() > 4) operands.erase(operands.begin()); continue; }
        const std::string& op = token.str;
        if (op == "begincodespacerange") {
            Obj a, b;
            while (parser.parse(a, false) && a.type == Obj::Type::String && parser.parse(b, false) && b.type == Obj::Type::String) {
                if (!a.str.empty() && a.str.size() <= 4) font.codespaces.push_back({static_cast<std::uint8_t>(a.str.size()), codeOf(a.str), codeOf(b.str)});
            }
        } else if (op == "beginbfchar") {
            Obj src, dst;
            while (parser.parse(src, false) && src.type == Obj::Type::String && parser.parse(dst, false)) {
                if (src.str.empty() || src.str.size() > 4) continue;
                std::string utf8;
                if (dst.type == Obj::Type::String) utf8 = utf16beToUtf8(dst.str);
                else if (dst.type == Obj::Type::Name) { std::string multi; const std::uint32_t cp = glyphNameToUnicode(dst.str, multi); if (cp == 0xFFFFFFFFu) utf8 = multi; else if (cp != 0) appendUtf8(utf8, cp); }
                font.map[(static_cast<std::uint64_t>(src.str.size()) << 32) | codeOf(src.str)] = std::move(utf8);
            }
        } else if (op == "beginbfrange") {
            Obj lo, hi, dst;
            while (parser.parse(lo, false) && lo.type == Obj::Type::String && parser.parse(hi, false) && parser.parse(dst, false)) {
                if (lo.str.empty() || lo.str.size() > 4) continue;
                const std::uint32_t l = codeOf(lo.str), h = codeOf(hi.str);
                const std::uint8_t len = static_cast<std::uint8_t>(lo.str.size());
                if (dst.type == Obj::Type::String) {
                    const std::string utf8 = utf16beToUtf8(dst.str);
                    // A single code point destination increments with the code; longer ones are kept as is.
                    std::uint32_t cp = 0;
                    std::size_t chars = 0;
                    for (std::size_t i = 0; i < utf8.size(); ++i) if ((static_cast<unsigned char>(utf8[i]) & 0xC0) != 0x80) ++chars;
                    if (chars == 1 && !utf8.empty()) {
                        const unsigned char c0 = static_cast<unsigned char>(utf8[0]);
                        if (c0 < 0x80) cp = c0;
                        else if (c0 < 0xE0 && utf8.size() >= 2) cp = ((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[1]) & 0x3Fu);
                        else if (c0 < 0xF0 && utf8.size() >= 3) cp = ((c0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(utf8[1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(utf8[2]) & 0x3Fu);
                        else if (utf8.size() >= 4) cp = ((c0 & 0x07u) << 18) | ((static_cast<unsigned char>(utf8[1]) & 0x3Fu) << 12) | ((static_cast<unsigned char>(utf8[2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(utf8[3]) & 0x3Fu);
                        if (h >= l && h - l < 0x10000) font.ranges.push_back({l, h, cp, len});
                    } else if (h >= l && h - l < 256) {
                        for (std::uint32_t code = l; code <= h; ++code) font.map[(static_cast<std::uint64_t>(len) << 32) | code] = utf8;
                    }
                } else if (dst.type == Obj::Type::Array) {
                    for (std::size_t i = 0; i < dst.items.size() && l + i <= h; ++i) {
                        if (dst.items[i].type == Obj::Type::String) font.map[(static_cast<std::uint64_t>(len) << 32) | (l + static_cast<std::uint32_t>(i))] = utf16beToUtf8(dst.items[i].str);
                    }
                }
            }
        }
        operands.clear();
    }
    font.hasToUnicode = !font.map.empty() || !font.ranges.empty();
}

// =============================================================================================== file

struct XrefEntry { int type = 0; std::uint64_t a = 0; std::uint32_t b = 0; };  // 1: offset a; 2: object stream a, index b


class PdfFile {
public:
    PdfFile(const ByteSource& source, Context& ctx) : source_(source), ctx_(ctx) {}

    enum class Open { Ok, Corrupt, Encrypted };

    Open open() {
        size_ = source_.size();
        if (size_ < 8) return Open::Corrupt;
        std::string head(static_cast<std::size_t>(std::min<std::uint64_t>(size_, 1024)), '\0');
        if (!source_.read(0, head.data(), head.size())) return Open::Corrupt;
        const std::size_t magic = head.find("%PDF-");
        if (magic == std::string::npos) return Open::Corrupt;
        base_ = magic;

        bool good = false;
        {
            const std::size_t tailSize = static_cast<std::size_t>(std::min<std::uint64_t>(size_, 4096));
            std::string tail(tailSize, '\0');
            if (source_.read(size_ - tailSize, tail.data(), tailSize)) {
                const std::size_t at = tail.rfind("startxref");
                if (at != std::string::npos) {
                    Parser p(tail, at + 9);
                    Obj offset;
                    if (p.parse(offset, false) && offset.type == Obj::Type::Int && offset.integer > 0) {
                        good = readXrefChain(static_cast<std::uint64_t>(offset.integer)) && rootIsUsable();
                    }
                }
            }
        }
        if (!good) {
            xref_.clear(); cache_.clear(); trailer_ = Obj{};
            rebuilt_ = true;
            if (!rebuild() || !rootIsUsable()) return Open::Corrupt;
        }
        if (trailer_.get("Encrypt") != nullptr) return Open::Encrypted;
        return Open::Ok;
    }

    const Obj& trailer() const { return trailer_; }

    // Resolves an indirect reference (or returns the object itself). Null if it cannot be loaded.
    const Obj* fetch(const Obj* o) {
        if (o == nullptr) return nullptr;
        if (o->type != Obj::Type::Ref) return o;
        const Obj* r = fetchNumber(static_cast<int>(o->integer));
        return (r != nullptr && r->type != Obj::Type::Null) ? r : nullptr;
    }
    const Obj* val(const Obj& dict, const char* key) { return fetch(dict.get(key)); }

    const Obj* fetchNumber(int number, int depth = 0) {
        if (number <= 0 || depth > 16) return nullptr;
        const auto cached = cache_.find(number);
        if (cached != cache_.end()) return cached->second.get();
        const auto entry = xref_.find(number);
        if (entry == xref_.end()) return nullptr;
        std::unique_ptr<Obj> loaded = std::make_unique<Obj>();
        bool ok = false;
        if (entry->second.type == 1) ok = loadAt(entry->second.a + base_, number, *loaded, true);
        else if (entry->second.type == 2) ok = loadFromObjectStream(static_cast<int>(entry->second.a), entry->second.b, number, *loaded, depth);
        if (!ok) { cache_[number] = nullptr; return nullptr; }
        loaded->number = number;
        const Obj* result = loaded.get();
        cache_[number] = std::move(loaded);
        return result;
    }

    // The decoded data of a stream object. `truncated` is set when the size limit cut it short.
    bool streamData(const Obj& stream, std::string& out, std::size_t maxOut, bool* partial = nullptr) {
        out.clear();
        if (!stream.isStream()) return false;
        std::string raw;
        if (!rawStream(stream, raw, maxOut + (maxOut >> 2))) return false;
        std::vector<std::string> filters;
        std::vector<const Obj*> params;
        const Obj* filter = fetch(stream.get("Filter"));
        if (filter == nullptr) filter = fetch(stream.get("F"));
        const Obj* decodeParms = fetch(stream.get("DecodeParms"));
        if (decodeParms == nullptr) decodeParms = fetch(stream.get("DP"));
        if (filter != nullptr && filter->type == Obj::Type::Name) {
            filters.push_back(filter->str);
            params.push_back(decodeParms != nullptr && decodeParms->type == Obj::Type::Dict ? decodeParms : (decodeParms != nullptr && decodeParms->type == Obj::Type::Array && !decodeParms->items.empty() ? fetch(&decodeParms->items[0]) : nullptr));
        } else if (filter != nullptr && filter->type == Obj::Type::Array) {
            for (std::size_t i = 0; i < filter->items.size(); ++i) {
                const Obj* f = fetch(&filter->items[i]);
                if (f == nullptr || f->type != Obj::Type::Name) continue;
                filters.push_back(f->str);
                const Obj* p = nullptr;
                if (decodeParms != nullptr && decodeParms->type == Obj::Type::Array && i < decodeParms->items.size()) p = fetch(&decodeParms->items[i]);
                else if (decodeParms != nullptr && decodeParms->type == Obj::Type::Dict && filter->items.size() == 1) p = decodeParms;
                params.push_back(p != nullptr && p->type == Obj::Type::Dict ? p : nullptr);
            }
        }
        for (std::size_t i = 0; i < filters.size(); ++i) {
            const std::string& f = filters[i];
            const Obj* parm = params[i];
            if (f == "FlateDecode" || f == "Fl") {
                std::string decoded;
                const InflateStatus s = inflateZlib(reinterpret_cast<const unsigned char*>(raw.data()), raw.size(), decoded, maxOut);
                if (s == InflateStatus::TooLarge && partial != nullptr) *partial = true;
                if (s == InflateStatus::Corrupt && decoded.empty()) return false;  // damaged from the start
                raw.swap(decoded);
                if (!applyPredictor(raw, parm)) return false;
            } else if (f == "LZWDecode" || f == "LZW") {
                int early = 1;
                if (parm != nullptr) { const Obj* e = fetch(parm->get("EarlyChange")); if (e != nullptr && e->isNum()) early = static_cast<int>(e->asNumber()); }
                raw = lzwDecode(raw, early, maxOut);
                if (!applyPredictor(raw, parm)) return false;
            } else if (f == "ASCII85Decode" || f == "A85") raw = ascii85Decode(raw);
            else if (f == "ASCIIHexDecode" || f == "AHx") raw = asciiHexDecode(raw);
            else if (f == "RunLengthDecode" || f == "RL") raw = runLengthDecode(raw);
            else return false;  // image codecs (DCT, JPX, CCITT, JBIG2) or a crypt filter: not text
            if (raw.size() > maxOut) { raw.resize(maxOut); if (partial != nullptr) *partial = true; }
        }
        out.swap(raw);
        return true;
    }

    const ByteSource& source() const { return source_; }
    bool rebuilt() const { return rebuilt_; }

private:
    bool applyPredictor(std::string& data, const Obj* parm) {
        if (parm == nullptr) return true;
        auto number = [&](const char* key, int fallback) {
            const Obj* v = fetch(parm->get(key));
            return v != nullptr && v->isNum() ? static_cast<int>(v->asNumber()) : fallback;
        };
        const int predictor = number("Predictor", 1);
        if (predictor < 10) return true;  // 1: none (TIFF predictor 2 is not used for text-bearing streams)
        const int columns = number("Columns", 1), colors = number("Colors", 1), bits = number("BitsPerComponent", 8);
        if (columns <= 0 || columns > (1 << 24) || colors <= 0 || colors > 32 || bits <= 0 || bits > 16) return false;
        applyPngPredictor(data, columns, colors, bits);
        return true;
    }

    // Reads `count` bytes at absolute offset, tolerating a short file.
    bool readAt(std::uint64_t offset, std::string& out, std::size_t count) {
        if (offset >= size_) { out.clear(); return false; }
        count = static_cast<std::size_t>(std::min<std::uint64_t>(count, size_ - offset));
        out.resize(count);
        return count == 0 || source_.read(offset, out.data(), count);
    }

    // Parses "N G obj <object> [stream]" at an absolute offset.
    bool loadAt(std::uint64_t offset, int expectNumber, Obj& out, bool allowStream) {
        std::size_t window = 64 * 1024;
        for (int attempt = 0; attempt < 5; ++attempt, window *= 4) {
            std::string buffer;
            if (!readAt(offset, buffer, window)) return false;
            Parser p(buffer);
            Obj number, gen, keyword;
            if (!p.parse(number, false) || number.type != Obj::Type::Int || !p.parse(gen, false) || gen.type != Obj::Type::Int ||
                !p.parse(keyword, false) || keyword.type != Obj::Type::Op || keyword.str.compare(0, 3, "obj") != 0) {
                return false;
            }
            if (expectNumber > 0 && number.integer != expectNumber) return false;
            if (keyword.str.size() > 3) p.setPos(p.pos() - (keyword.str.size() - 3));  // "obj<<" with no space
            Obj object;
            if (!p.parse(object, true)) {
                if (buffer.size() >= size_ - offset || window >= 16u * 1024 * 1024) return false;
                continue;  // object larger than the window: read more
            }
            if (allowStream && object.type == Obj::Type::Dict) {
                p.skipWhite();
                if (buffer.compare(p.pos(), 6, "stream") == 0) {
                    std::size_t at = p.pos() + 6;
                    if (at < buffer.size() && buffer[at] == '\r') ++at;
                    if (at < buffer.size() && buffer[at] == '\n') ++at;
                    object.streamPos = static_cast<std::int64_t>(offset + at);
                }
            }
            out = std::move(object);
            return true;
        }
        return false;
    }

    bool rawStream(const Obj& stream, std::string& raw, std::size_t maxRaw) {
        std::int64_t length = -1;
        if (const Obj* l = fetch(stream.get("Length")); l != nullptr && l->type == Obj::Type::Int) length = l->integer;
        const std::uint64_t start = static_cast<std::uint64_t>(stream.streamPos);
        auto endsProperly = [&](std::uint64_t len) {
            std::string probe;
            if (!readAt(start + len, probe, 16)) return false;
            std::size_t i = 0;
            while (i < probe.size() && isWhite(probe[i])) ++i;
            return probe.compare(i, 9, "endstream") == 0;
        };
        if (length < 0 || static_cast<std::uint64_t>(length) > size_ || start + static_cast<std::uint64_t>(length) > size_ || !endsProperly(static_cast<std::uint64_t>(length))) {
            // Wrong or missing /Length: look for the end marker.
            std::uint64_t found = 0;
            bool ok = false;
            std::string chunk;
            const std::size_t kChunk = 1 << 20;
            for (std::uint64_t at = start; at < size_ && !ctx_.budget.expired(); at += kChunk - 16) {
                if (!readAt(at, chunk, kChunk)) break;
                const std::size_t k = chunk.find("endstream");
                if (k != std::string::npos) { found = at + k - start; ok = true; break; }
                if (at + chunk.size() >= size_) break;
                if (at - start > maxRaw) break;
            }
            if (!ok) return false;
            length = static_cast<std::int64_t>(found);
            // Drop the end-of-line before the marker.
            std::string tail;
            if (length > 0 && readAt(start + static_cast<std::uint64_t>(length) - std::min<std::int64_t>(length, 2), tail, static_cast<std::size_t>(std::min<std::int64_t>(length, 2)))) {
                if (!tail.empty() && tail.back() == '\n') --length;
                if (length > 0 && tail.size() == 2 && tail[0] == '\r' && tail[1] == '\n') --length;
                else if (!tail.empty() && tail.back() == '\r') --length;
            }
        }
        if (static_cast<std::uint64_t>(length) > maxRaw) return false;
        return readAt(start, raw, static_cast<std::size_t>(length)) && raw.size() == static_cast<std::size_t>(length);
    }

    bool loadFromObjectStream(int streamNumber, std::uint32_t index, int wanted, Obj& out, int depth) {
        if (streamNumber == wanted) return false;
        const Obj* stream = fetchNumber(streamNumber, depth + 1);
        if (stream == nullptr || !stream->isStream()) return false;
        auto cached = std::find_if(objectStreams_.begin(), objectStreams_.end(), [&](const DecodedObjStm& d) { return d.number == streamNumber; });
        if (cached == objectStreams_.end()) {
            DecodedObjStm decoded;
            decoded.number = streamNumber;
            if (!streamData(*stream, decoded.data, ctx_.limits.maxStreamBytes)) return false;
            const Obj* n = fetch(stream->get("N"));
            const Obj* first = fetch(stream->get("First"));
            if (n == nullptr || first == nullptr || !n->isNum() || !first->isNum()) return false;
            decoded.first = static_cast<std::size_t>(first->asNumber());
            Parser p(decoded.data);
            for (int i = 0; i < static_cast<int>(n->asNumber()) && i < 1000000; ++i) {
                Obj num, off;
                if (!p.parse(num, false) || !p.parse(off, false) || num.type != Obj::Type::Int || off.type != Obj::Type::Int) break;
                decoded.entries.push_back({static_cast<int>(num.integer), static_cast<std::size_t>(off.integer)});
            }
            if (objectStreams_.size() >= 6) objectStreams_.erase(objectStreams_.begin());
            objectStreams_.push_back(std::move(decoded));
            cached = objectStreams_.end() - 1;
        }
        std::size_t slot = index;
        if (slot >= cached->entries.size() || cached->entries[slot].first != wanted) {
            slot = cached->entries.size();
            for (std::size_t i = 0; i < cached->entries.size(); ++i) if (cached->entries[i].first == wanted) { slot = i; break; }
            if (slot == cached->entries.size()) return false;
        }
        Parser p(cached->data, cached->first + cached->entries[slot].second);
        return p.parse(out, true);
    }

    bool readXrefChain(std::uint64_t firstOffset) {
        std::set<std::uint64_t> visited;
        std::vector<std::uint64_t> pending{firstOffset};
        bool anyTrailer = false;
        while (!pending.empty()) {
            const std::uint64_t offset = pending.back();
            pending.pop_back();
            if (!visited.insert(offset).second || visited.size() > 4096) continue;
            Obj dict;
            std::uint64_t at = offset + base_;
            std::string probe;
            if (!readAt(at, probe, 64)) return anyTrailer;
            std::size_t skip = 0;
            while (skip < probe.size() && isWhite(probe[skip])) ++skip;
            if (probe.compare(skip, 4, "xref") == 0) {
                if (!readXrefTable(at + skip + 4, dict)) return false;
            } else {
                if (!readXrefStream(at, dict)) return false;
            }
            if (!anyTrailer) { trailer_ = dict; anyTrailer = true; }
            else {
                for (std::size_t i = 0; i < dict.keys.size(); ++i) {
                    if (trailer_.get(dict.keys[i].c_str()) == nullptr) { trailer_.keys.push_back(dict.keys[i]); trailer_.items.push_back(dict.items[i]); }
                }
            }
            if (const Obj* prev = dict.get("Prev"); prev != nullptr && prev->isNum()) pending.push_back(static_cast<std::uint64_t>(prev->asNumber()));
            if (const Obj* stm = dict.get("XRefStm"); stm != nullptr && stm->isNum()) pending.push_back(static_cast<std::uint64_t>(stm->asNumber()));
        }
        return anyTrailer;
    }

    bool readXrefTable(std::uint64_t offset, Obj& trailer) {
        std::uint64_t at = offset;
        for (int sections = 0; sections < 100000; ++sections) {
            std::string buffer;
            if (!readAt(at, buffer, 256 * 1024)) return false;
            Parser p(buffer);
            p.skipWhite();
            if (buffer.compare(p.pos(), 7, "trailer") == 0) {
                p.setPos(p.pos() + 7);
                return p.parse(trailer, true) && trailer.type == Obj::Type::Dict;
            }
            Obj start, count;
            if (!p.parse(start, false) || !p.parse(count, false) || start.type != Obj::Type::Int || count.type != Obj::Type::Int || count.integer < 0 || count.integer > 50000000) return false;
            std::uint64_t entryAt = at + p.pos();
            // Entries are 20 bytes each, but be lenient about the exact line ending.
            const std::int64_t n = count.integer;
            std::string entries;
            std::int64_t consumedEntries = 0;
            std::uint64_t cursor = entryAt;
            while (consumedEntries < n) {
                const std::int64_t batch = std::min<std::int64_t>(n - consumedEntries, 8192);
                if (!readAt(cursor, entries, static_cast<std::size_t>(batch) * 22 + 64)) return false;
                Parser ep(entries);
                std::int64_t done = 0;
                for (; done < batch; ++done) {
                    Obj off, gen, flag;
                    const std::size_t before = ep.pos();
                    if (!ep.parse(off, false) || !ep.parse(gen, false) || !ep.parse(flag, false) || off.type != Obj::Type::Int || gen.type != Obj::Type::Int || flag.type != Obj::Type::Op) { ep.setPos(before); break; }
                    const std::int64_t number = start.integer + consumedEntries + done;
                    if (flag.str == "n" && off.integer > 0 && number > 0 && xref_.find(static_cast<int>(number)) == xref_.end()) {
                        xref_[static_cast<int>(number)] = {1, static_cast<std::uint64_t>(off.integer), static_cast<std::uint32_t>(gen.integer)};
                    } else if (flag.str == "f" && number > 0 && xref_.find(static_cast<int>(number)) == xref_.end()) {
                        xref_[static_cast<int>(number)] = {0, 0, 0};
                    }
                }
                if (done == 0) return false;
                consumedEntries += done;
                cursor += ep.pos();
                if (ctx_.budget.expired()) return false;
            }
            at = cursor;
        }
        return false;
    }

    bool readXrefStream(std::uint64_t offset, Obj& dict) {
        Obj object;
        if (!loadAt(offset, 0, object, true) || !object.isStream()) return false;
        if (!object.isName("XRef") && (object.get("Type") == nullptr || !object.get("Type")->isName("XRef"))) return false;
        std::string data;
        if (!streamData(object, data, ctx_.limits.maxStreamBytes)) return false;
        const Obj* w = object.get("W");
        const Obj* size = object.get("Size");
        if (w == nullptr || w->type != Obj::Type::Array || w->items.size() < 3 || size == nullptr || !size->isNum()) return false;
        int widths[3];
        for (int i = 0; i < 3; ++i) { widths[i] = static_cast<int>(w->items[static_cast<std::size_t>(i)].asNumber()); if (widths[i] < 0 || widths[i] > 8) return false; }
        const std::size_t rowSize = static_cast<std::size_t>(widths[0] + widths[1] + widths[2]);
        if (rowSize == 0) return false;
        std::vector<std::pair<std::int64_t, std::int64_t>> sections;
        if (const Obj* index = object.get("Index"); index != nullptr && index->type == Obj::Type::Array) {
            for (std::size_t i = 0; i + 1 < index->items.size(); i += 2) sections.push_back({static_cast<std::int64_t>(index->items[i].asNumber()), static_cast<std::int64_t>(index->items[i + 1].asNumber())});
        } else {
            sections.push_back({0, static_cast<std::int64_t>(size->asNumber())});
        }
        std::size_t pos = 0;
        auto field = [&](int width, std::uint64_t fallback) {
            if (width == 0) return fallback;
            std::uint64_t v = 0;
            for (int k = 0; k < width; ++k) v = (v << 8) | static_cast<unsigned char>(data[pos++]);
            return v;
        };
        for (const auto& section : sections) {
            for (std::int64_t i = 0; i < section.second; ++i) {
                if (pos + rowSize > data.size()) break;
                const std::uint64_t type = field(widths[0], 1);
                const std::uint64_t a = field(widths[1], 0);
                const std::uint64_t b = field(widths[2], 0);
                const int number = static_cast<int>(section.first + i);
                if (number <= 0 || xref_.find(number) != xref_.end()) continue;
                if (type == 1) xref_[number] = {1, a, static_cast<std::uint32_t>(b)};
                else if (type == 2) xref_[number] = {2, a, static_cast<std::uint32_t>(b)};
                else xref_[number] = {0, 0, 0};
            }
        }
        dict = object;
        dict.streamPos = -1;
        return true;
    }

    bool rootIsUsable() {
        const Obj* rootRef = trailer_.get("Root");
        if (rootRef == nullptr) return false;
        const Obj* root = fetch(rootRef);
        return root != nullptr && root->type == Obj::Type::Dict && root->get("Pages") != nullptr;
    }

    // No usable cross-reference data: find the objects by scanning the whole file.
    bool rebuild() {
        std::int64_t catalog = 0;
        std::vector<int> objectStreams;
        std::unordered_map<int, std::uint64_t> found;
        std::string chunk;
        const std::size_t kChunk = 4 * 1024 * 1024, kOverlap = 64;
        for (std::uint64_t at = 0; at < size_; at += kChunk - kOverlap) {
            if (ctx_.budget.expired()) return false;
            if (!readAt(at, chunk, kChunk)) break;
            for (std::size_t i = 0; i + 3 <= chunk.size();) {
                const std::size_t k = chunk.find("obj", i);
                if (k == std::string::npos) break;
                i = k + 3;
                if (k >= 3 && chunk.compare(k - 3, 3, "end") == 0) continue;
                // Walk back over "N G " before the keyword.
                std::size_t p = k;
                while (p > 0 && isWhite(chunk[p - 1])) --p;
                const std::size_t genEnd = p;
                while (p > 0 && std::isdigit(static_cast<unsigned char>(chunk[p - 1]))) --p;
                if (p == genEnd) continue;
                std::size_t q = p;
                while (q > 0 && isWhite(chunk[q - 1])) --q;
                if (q == p) continue;
                const std::size_t numEnd = q;
                while (q > 0 && std::isdigit(static_cast<unsigned char>(chunk[q - 1]))) --q;
                if (q == numEnd || numEnd - q > 9) continue;
                if (q > 0 && !isWhite(chunk[q - 1]) && !isDelim(chunk[q - 1]) && chunk.compare(q >= 6 ? q - 6 : 0, 6, "endobj") != 0) {
                    if (!(q >= 9 && chunk.compare(q - 9, 9, "endstream") == 0)) continue;
                }
                if (at > 0 && q < kOverlap) continue;  // seen as part of the previous chunk
                const int number = std::atoi(chunk.substr(q, numEnd - q).c_str());
                if (number <= 0) continue;
                found[number] = at + q;
                const std::string_view window(chunk.data() + k, std::min<std::size_t>(300, chunk.size() - k));
                if (window.find("/Catalog") != std::string_view::npos && window.find("/Pages") != std::string_view::npos) catalog = number;
                if (window.find("/ObjStm") != std::string_view::npos) objectStreams.push_back(number);
                if (window.find("/XRef") != std::string_view::npos) {
                    Obj dict;
                    if (loadAtRaw(at + q, dict) && dict.get("Root") != nullptr) { trailer_ = dict; trailer_.streamPos = -1; }
                }
            }
            // "trailer" dictionaries
            for (std::size_t t = chunk.find("trailer"); t != std::string::npos; t = chunk.find("trailer", t + 7)) {
                Parser p(chunk, t + 7);
                Obj dict;
                if (p.parse(dict, true) && dict.type == Obj::Type::Dict && dict.get("Root") != nullptr) trailer_ = dict;
            }
            if (at + chunk.size() >= size_) break;
        }
        for (const auto& f : found) xref_[f.first] = {1, f.second, 0};
        for (const int stream : objectStreams) {
            const Obj* os = fetchNumber(stream);
            if (os == nullptr || !os->isStream()) continue;
            std::string data;
            if (!streamData(*os, data, ctx_.limits.maxStreamBytes)) continue;
            const Obj* n = fetch(os->get("N"));
            if (n == nullptr || !n->isNum()) continue;
            Parser p(data);
            for (int i = 0; i < static_cast<int>(n->asNumber()) && i < 1000000; ++i) {
                Obj num, off;
                if (!p.parse(num, false) || !p.parse(off, false) || num.type != Obj::Type::Int) break;
                if (xref_.find(static_cast<int>(num.integer)) == xref_.end()) {
                    xref_[static_cast<int>(num.integer)] = {2, static_cast<std::uint64_t>(stream), static_cast<std::uint32_t>(i)};
                    if (catalog == 0) {
                        const Obj* candidate = fetchNumber(static_cast<int>(num.integer));
                        if (candidate != nullptr && candidate->type == Obj::Type::Dict && candidate->get("Type") != nullptr && candidate->get("Type")->isName("Catalog")) catalog = num.integer;
                    }
                }
            }
        }
        if (trailer_.get("Root") == nullptr && catalog != 0) {
            trailer_.type = Obj::Type::Dict;
            Obj ref;
            ref.type = Obj::Type::Ref;
            ref.integer = catalog;
            trailer_.keys.push_back("Root");
            trailer_.items.push_back(ref);
        }
        return trailer_.get("Root") != nullptr;
    }

    bool loadAtRaw(std::uint64_t offset, Obj& out) { return loadAt(offset, 0, out, false); }

    struct DecodedObjStm {
        int number = 0;
        std::string data;
        std::size_t first = 0;
        std::vector<std::pair<int, std::size_t>> entries;
    };

    const ByteSource& source_;
    Context& ctx_;
    std::uint64_t size_ = 0;
    std::uint64_t base_ = 0;
    bool rebuilt_ = false;
    Obj trailer_;
    std::unordered_map<int, XrefEntry> xref_;
    std::unordered_map<int, std::unique_ptr<Obj>> cache_;
    std::vector<DecodedObjStm> objectStreams_;
};

// ============================================================================================ interpreter

struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};
Matrix multiply(const Matrix& m, const Matrix& n) {  // m applied first, then n
    return {m.a * n.a + m.b * n.c, m.a * n.b + m.b * n.d, m.c * n.a + m.d * n.c, m.c * n.b + m.d * n.d,
            m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f};
}

class PageReader {
public:
    PageReader(PdfFile& file, Context& ctx) : file_(file), ctx_(ctx) {}

    // Interprets one page's content; returns its text. `images` is set when the page draws images.
    std::string readPage(const Obj& page, const Obj* inheritedResources, bool& images, bool& unmappable) {
        text_.clear();
        images_ = false;
        unmappable_ = false;
        haveLast_ = false;
        const Obj* resources = file_.val(page, "Resources");
        if (resources == nullptr) resources = inheritedResources;
        const Obj* contents = file_.fetch(page.get("Contents"));
        if (contents != nullptr && resources != nullptr) {
            std::string combined;
            auto addStream = [&](const Obj* s) {
                std::string data;
                if (s != nullptr && s->isStream() && file_.streamData(*s, data, ctx_.limits.maxStreamBytes) && combined.size() + data.size() < 2 * ctx_.limits.maxStreamBytes) {
                    combined += data;
                    combined.push_back('\n');
                }
            };
            if (contents->type == Obj::Type::Array) {
                for (const Obj& item : contents->items) addStream(file_.fetch(&item));
            } else {
                addStream(contents);
            }
            Matrix ctm;
            run(combined, *resources, ctm, 0);
        }
        images = images_;
        unmappable = unmappable_;
        return std::move(text_);
    }

private:
    struct State {
        Matrix ctm;
        Matrix tm, tlm;
        const Font* font = nullptr;
        double size = 0, tc = 0, tw = 0, th = 1, tl = 0;
    };

    const Font* loadFont(const Obj& resources, const std::string& name) {
        const Obj* fonts = file_.val(resources, "Font");
        if (fonts == nullptr || fonts->type != Obj::Type::Dict) return nullptr;
        const Obj* dict = file_.val(*fonts, name.c_str());
        if (dict == nullptr || dict->type != Obj::Type::Dict) return nullptr;
        const auto it = fontCache_.find(dict);
        if (it != fontCache_.end()) return it->second.get();
        auto font = std::make_unique<Font>();
        buildFont(*dict, *font);
        const Font* result = font.get();
        fontCache_[dict] = std::move(font);
        return result;
    }

    void buildFont(const Obj& dict, Font& font) {
        const Obj* subtype = file_.val(dict, "Subtype");
        font.composite = subtype != nullptr && subtype->isName("Type0");
        BaseEncoding base = BaseEncoding::Standard;
        bool haveBase = false;
        const Obj* encoding = file_.val(dict, "Encoding");
        if (encoding != nullptr && encoding->type == Obj::Type::Name) {
            if (encoding->str == "WinAnsiEncoding") { base = BaseEncoding::WinAnsi; haveBase = true; }
            else if (encoding->str == "MacRomanEncoding") { base = BaseEncoding::MacRoman; haveBase = true; }
            else if (font.composite && (encoding->str.find("UCS2") != std::string::npos || encoding->str.find("UTF16") != std::string::npos)) font.unicodeCodes = true;
        } else if (encoding != nullptr && encoding->type == Obj::Type::Dict) {
            const Obj* b = file_.val(*encoding, "BaseEncoding");
            if (b != nullptr && b->isName("WinAnsiEncoding")) { base = BaseEncoding::WinAnsi; haveBase = true; }
            else if (b != nullptr && b->isName("MacRomanEncoding")) { base = BaseEncoding::MacRoman; haveBase = true; }
        }
        if (!font.composite) {
            fillBaseEncoding(font.encoding, haveBase ? base : BaseEncoding::WinAnsi);
            if (encoding != nullptr && encoding->type == Obj::Type::Dict) {
                const Obj* diffs = file_.val(*encoding, "Differences");
                if (diffs != nullptr && diffs->type == Obj::Type::Array) {
                    int code = 0;
                    for (const Obj& item : diffs->items) {
                        const Obj* v = file_.fetch(&item);
                        if (v == nullptr) continue;
                        if (v->isNum()) code = static_cast<int>(v->asNumber());
                        else if (v->type == Obj::Type::Name && code >= 0 && code < 256) {
                            std::string multi;
                            const std::uint32_t cp = glyphNameToUnicode(v->str, multi);
                            if (cp != 0 && cp != 0xFFFFFFFFu) font.encoding[code] = cp;
                            else if (cp == 0xFFFFFFFFu) font.map[(std::uint64_t{1} << 32) | static_cast<unsigned>(code)] = multi;
                            else font.encoding[code] = 0;
                            ++code;
                        }
                    }
                }
            }
        }
        if (const Obj* toUnicode = file_.fetch(dict.get("ToUnicode")); toUnicode != nullptr && toUnicode->isStream()) {
            std::string cmap;
            if (file_.streamData(*toUnicode, cmap, 8 * 1024 * 1024)) parseCMap(cmap, font);
        }
        // Widths.
        if (font.composite) {
            const Obj* descendants = file_.val(dict, "DescendantFonts");
            const Obj* cid = descendants != nullptr && descendants->type == Obj::Type::Array && !descendants->items.empty() ? file_.fetch(&descendants->items[0]) : nullptr;
            if (cid != nullptr && cid->type == Obj::Type::Dict) {
                if (const Obj* dw = file_.val(*cid, "DW"); dw != nullptr && dw->isNum()) font.defaultWidth = dw->asNumber();
                if (const Obj* w = file_.val(*cid, "W"); w != nullptr && w->type == Obj::Type::Array) {
                    const auto& items = w->items;
                    for (std::size_t i = 0; i < items.size() && font.cidWidths.size() < 200000;) {
                        const Obj* first = file_.fetch(&items[i]);
                        if (first == nullptr || !first->isNum() || i + 1 >= items.size()) break;
                        const Obj* second = file_.fetch(&items[i + 1]);
                        if (second != nullptr && second->type == Obj::Type::Array) {
                            std::uint32_t c = static_cast<std::uint32_t>(first->asNumber());
                            for (const Obj& wi : second->items) { const Obj* wv = file_.fetch(&wi); if (wv != nullptr && wv->isNum()) font.cidWidths[c] = wv->asNumber(); ++c; }
                            i += 2;
                        } else if (second != nullptr && second->isNum() && i + 2 < items.size()) {
                            const Obj* width = file_.fetch(&items[i + 2]);
                            const std::uint32_t lo = static_cast<std::uint32_t>(first->asNumber()), hi = static_cast<std::uint32_t>(second->asNumber());
                            if (width != nullptr && width->isNum() && hi >= lo && hi - lo < 70000) for (std::uint32_t c = lo; c <= hi; ++c) font.cidWidths[c] = width->asNumber();
                            i += 3;
                        } else break;
                    }
                }
            }
            if (font.codespaces.empty() && !font.hasToUnicode) font.unmappable = !font.unicodeCodes;
            if (!font.hasToUnicode && !font.unicodeCodes) font.unmappable = true;
        } else {
            if (subtype != nullptr && subtype->isName("Type3")) {
                if (const Obj* fm = file_.val(dict, "FontMatrix"); fm != nullptr && fm->type == Obj::Type::Array && !fm->items.empty()) {
                    const Obj* a = file_.fetch(&fm->items[0]);
                    if (a != nullptr && a->isNum() && a->asNumber() > 0) font.widthScale = a->asNumber() * 1000.0;
                }
            }
            if (const Obj* fc = file_.val(dict, "FirstChar"); fc != nullptr && fc->isNum()) font.firstChar = static_cast<int>(fc->asNumber());
            if (const Obj* w = file_.val(dict, "Widths"); w != nullptr && w->type == Obj::Type::Array) {
                for (const Obj& item : w->items) { const Obj* v = file_.fetch(&item); font.widths.push_back(v != nullptr && v->isNum() ? v->asNumber() : 0); }
            }
            if (const Obj* fd = file_.val(dict, "FontDescriptor"); fd != nullptr && fd->type == Obj::Type::Dict) {
                if (const Obj* mw = file_.val(*fd, "MissingWidth"); mw != nullptr && mw->isNum()) font.missingWidth = mw->asNumber();
            }
        }
    }

    // Decodes a shown string into text and returns the advance in text-space units (before Tz).
    double decode(const Font& font, const std::string& s, const State& st, std::string& out) {
        double advance = 0;
        std::size_t i = 0;
        while (i < s.size()) {
            std::uint32_t code = static_cast<unsigned char>(s[i]);
            std::size_t length = 1;
            if (font.composite) {
                length = 0;
                for (std::size_t l = 1; l <= 4 && i + l <= s.size() && length == 0; ++l) {
                    std::uint32_t candidate = 0;
                    for (std::size_t k = 0; k < l; ++k) candidate = (candidate << 8) | static_cast<unsigned char>(s[i + k]);
                    for (const Font::Space& space : font.codespaces) {
                        if (space.length == l && codeInSpace(space, candidate)) { length = l; code = candidate; break; }
                    }
                }
                if (length == 0) {
                    length = font.codespaces.empty() ? 2 : font.codespaces.front().length;
                    if (i + length > s.size()) length = s.size() - i;
                    code = 0;
                    for (std::size_t k = 0; k < length; ++k) code = (code << 8) | static_cast<unsigned char>(s[i + k]);
                }
            }
            std::string mapped;
            bool known = false;
            const auto it = font.map.find((static_cast<std::uint64_t>(length) << 32) | code);
            if (it != font.map.end()) { mapped = it->second; known = true; }
            else if (!font.ranges.empty()) {
                for (const Font::Range& r : font.ranges) {
                    if (r.length == length && code >= r.lo && code <= r.hi) { appendUtf8(mapped, r.cp + (code - r.lo)); known = true; break; }
                }
            }
            if (!known) {
                if (font.composite) {
                    if (font.unicodeCodes) { appendUtf8(mapped, code); known = true; }
                } else if (font.encoding[code & 0xFF] != 0) { appendUtf8(mapped, font.encoding[code & 0xFF]); known = true; }
            }
            if (known) appendLigatureSafe(out, mapped);
            else unmappable_ = true;

            double width;
            if (font.composite) {
                const auto w = font.cidWidths.find(code);
                if (w != font.cidWidths.end() && w->second > 0) {
                    width = w->second;
                } else if (w == font.cidWidths.end() && font.defaultWidth > 0) {
                    width = font.defaultWidth;
                } else {
                    // The file does not say how wide this glyph is (a /DW of 0 is common in browser-made PDFs). The
                    // position of the next glyph then tells nothing about word breaks: see show().
                    widthUnknown_ = true;
                    width = 500;
                }
            } else {
                const int index = static_cast<int>(code) - font.firstChar;
                if (index >= 0 && static_cast<std::size_t>(index) < font.widths.size() && font.widths[static_cast<std::size_t>(index)] > 0) width = font.widths[static_cast<std::size_t>(index)] * font.widthScale;
                else width = font.missingWidth > 0 ? font.missingWidth * font.widthScale : (code == 32 ? 250 : 500);
            }
            advance += width / 1000.0 * st.size + st.tc + ((length == 1 && code == 32) ? st.tw : 0);
            i += length;
        }
        return advance;
    }

    // Writes a shown string, deciding from the position whether it starts a new line or a new word.
    void show(State& st, const std::string& bytes) {
        if (st.font == nullptr) return;
        std::string decoded;
        widthUnknown_ = false;
        const double advance = decode(*st.font, bytes, st, decoded);
        const bool unknownWidth = widthUnknown_;
        const Matrix device = multiply(st.tm, st.ctm);
        const double x = device.e, y = device.f;
        const double scaleY = std::hypot(st.tm.c, st.tm.d) * std::hypot(st.ctm.c, st.ctm.d);
        const double fontSize = std::max(1.0, std::fabs(st.size) * scaleY);
        if (haveLast_ && !decoded.empty()) {
            const double dy = std::fabs(y - lastY_);
            if (dy > fontSize * 0.6) {
                if (!text_.empty() && text_.back() != '\n') text_.push_back('\n');
            } else if (!prevWidthUnknown_) {
                // Same line: a gap wider than a typical kerning adjustment is a word break. (When the previous glyph's
                // width was unknown the gap is just its width, so such files are trusted to contain real space glyphs.)
                const double gap = x - lastX_;
                if (gap > fontSize * 0.17 || gap < -fontSize * 0.5) {
                    if (!text_.empty() && text_.back() != ' ' && text_.back() != '\n' && !decoded.empty() && decoded[0] != ' ') text_.push_back(' ');
                }
            }
        }
        text_ += decoded;
        // Advance the text matrix by the width of the string.
        st.tm.e += advance * st.th * st.tm.a;
        st.tm.f += advance * st.th * st.tm.b;
        const Matrix after = multiply(st.tm, st.ctm);
        lastX_ = after.e;
        lastY_ = after.f;
        if (!decoded.empty()) {
            haveLast_ = true;
            prevWidthUnknown_ = unknownWidth;
        }
        if (text_.size() > 4 * ctx_.limits.maxTextBytes) text_.resize(4 * ctx_.limits.maxTextBytes);
    }

    void lineMove(State& st, double tx, double ty) {
        st.tlm = multiply(Matrix{1, 0, 0, 1, tx, ty}, st.tlm);
        st.tm = st.tlm;
    }

    void run(const std::string& content, const Obj& resources, const Matrix& startCtm, int depth) {
        if (depth > 6) return;
        Parser parser(content);
        std::vector<Obj> operands;
        State st;
        st.ctm = startCtm;
        std::vector<State> saved;
        Obj token;
        auto num = [&](std::size_t i) { return i < operands.size() && operands[i].isNum() ? operands[i].asNumber() : 0.0; };
        std::size_t guard = 0;
        for (;;) {
            if (++guard % 512 == 0 && ctx_.budget.expired()) return;
            double number;
            if (parser.fastNumber(number)) {
                Obj n;
                n.type = Obj::Type::Real;
                n.num = number;
                operands.push_back(std::move(n));
                if (operands.size() > 64) operands.erase(operands.begin());
                continue;
            }
            std::string_view word;
            if (parser.fastWord(word)) {
                if (word == "true" || word == "false" || word == "null") { operands.push_back(Obj{}); continue; }
                token = Obj{};
                token.type = Obj::Type::Op;
                token.str.assign(word);
            } else if (!parser.parse(token, false)) {
                break;
            }
            if (token.type != Obj::Type::Op) {
                operands.push_back(std::move(token));
                if (operands.size() > 64) operands.erase(operands.begin());
                continue;
            }
            const std::string& op = token.str;
            if (op == "q") { if (saved.size() < 64) saved.push_back(st); }
            else if (op == "Q") { if (!saved.empty()) { const Matrix keepTm = st.tm, keepTlm = st.tlm; st = saved.back(); saved.pop_back(); st.tm = keepTm; st.tlm = keepTlm; } }
            else if (op == "cm" && operands.size() >= 6) {
                const std::size_t o = operands.size() - 6;
                st.ctm = multiply(Matrix{num(o), num(o + 1), num(o + 2), num(o + 3), num(o + 4), num(o + 5)}, st.ctm);
            }
            else if (op == "BT") { st.tm = Matrix{}; st.tlm = Matrix{}; }
            else if (op == "Tc" && !operands.empty()) st.tc = num(operands.size() - 1);
            else if (op == "Tw" && !operands.empty()) st.tw = num(operands.size() - 1);
            else if (op == "Tz" && !operands.empty()) st.th = num(operands.size() - 1) / 100.0;
            else if (op == "TL" && !operands.empty()) st.tl = num(operands.size() - 1);
            else if (op == "Tf" && operands.size() >= 2) {
                const Obj& name = operands[operands.size() - 2];
                st.size = num(operands.size() - 1);
                st.font = name.type == Obj::Type::Name ? loadFont(resources, name.str) : nullptr;
            }
            else if ((op == "Td" || op == "TD") && operands.size() >= 2) {
                const double tx = num(operands.size() - 2), ty = num(operands.size() - 1);
                if (op == "TD") st.tl = -ty;
                lineMove(st, tx, ty);
            }
            else if (op == "Tm" && operands.size() >= 6) {
                const std::size_t o = operands.size() - 6;
                st.tm = st.tlm = Matrix{num(o), num(o + 1), num(o + 2), num(o + 3), num(o + 4), num(o + 5)};
            }
            else if (op == "T*") lineMove(st, 0, -st.tl);
            else if (op == "Tj" && !operands.empty() && operands.back().type == Obj::Type::String) show(st, operands.back().str);
            else if ((op == "'" || op == "\"") && !operands.empty() && operands.back().type == Obj::Type::String) {
                if (op == "\"" && operands.size() >= 3) { st.tw = num(operands.size() - 3); st.tc = num(operands.size() - 2); }
                lineMove(st, 0, -st.tl);
                show(st, operands.back().str);
            }
            else if (op == "TJ" && !operands.empty() && operands.back().type == Obj::Type::Array) {
                for (const Obj& item : operands.back().items) {
                    if (item.type == Obj::Type::String) show(st, item.str);
                    else if (item.isNum()) {
                        const double shift = -item.asNumber() / 1000.0 * st.size * st.th;
                        st.tm.e += shift * st.tm.a;
                        st.tm.f += shift * st.tm.b;
                    }
                }
            }
            else if (op == "Do" && !operands.empty() && operands.back().type == Obj::Type::Name) {
                const Obj* xobjects = file_.val(resources, "XObject");
                const Obj* xo = xobjects != nullptr && xobjects->type == Obj::Type::Dict ? file_.val(*xobjects, operands.back().str.c_str()) : nullptr;
                if (xo != nullptr && xo->isStream()) {
                    const Obj* subtype = file_.val(*xo, "Subtype");
                    if (subtype != nullptr && subtype->isName("Image")) images_ = true;
                    else if (subtype != nullptr && subtype->isName("Form") && activeForms_.insert(xo->number).second) {
                        std::string data;
                        if (file_.streamData(*xo, data, ctx_.limits.maxStreamBytes)) {
                            Matrix m = st.ctm;
                            if (const Obj* mx = file_.val(*xo, "Matrix"); mx != nullptr && mx->type == Obj::Type::Array && mx->items.size() >= 6) {
                                m = multiply(Matrix{mx->items[0].asNumber(), mx->items[1].asNumber(), mx->items[2].asNumber(), mx->items[3].asNumber(), mx->items[4].asNumber(), mx->items[5].asNumber()}, st.ctm);
                            }
                            const Obj* inner = file_.val(*xo, "Resources");
                            run(data, inner != nullptr ? *inner : resources, m, depth + 1);
                        }
                        activeForms_.erase(xo->number);
                    }
                }
            }
            else if (op == "BI") {  // inline image: skip to "EI"
                images_ = true;
                std::size_t at = parser.pos();
                const std::string_view d = parser.data();
                std::size_t id = d.find("ID", at);
                std::size_t ei = id == std::string_view::npos ? id : id + 2;
                while (ei != std::string_view::npos) {
                    ei = d.find("EI", ei);
                    if (ei == std::string_view::npos) break;
                    if ((ei == 0 || isWhite(d[ei - 1])) && (ei + 2 >= d.size() || isWhite(d[ei + 2]))) break;
                    ei += 2;
                }
                parser.setPos(ei == std::string_view::npos ? d.size() : ei + 2);
            }
            operands.clear();
        }
    }

    PdfFile& file_;
    Context& ctx_;
    std::string text_;
    bool images_ = false, unmappable_ = false, haveLast_ = false, widthUnknown_ = false, prevWidthUnknown_ = false;
    double lastX_ = 0, lastY_ = 0;
    std::unordered_map<const Obj*, std::unique_ptr<Font>> fontCache_;
    std::set<std::int64_t> activeForms_;
};

// PDF text strings: UTF-16BE with a BOM, otherwise (approximately) Latin-1.
std::string pdfTextString(const std::string& s) {
    if (s.size() >= 2 && static_cast<unsigned char>(s[0]) == 0xFE && static_cast<unsigned char>(s[1]) == 0xFF) {
        return utf16ToUtf8(reinterpret_cast<const unsigned char*>(s.data()) + 2, s.size() - 2, true);
    }
    return isValidUtf8(s) ? s : latin1ToUtf8(s);
}

}  // namespace

ExtractStatus extractPdf(const ByteSource& source, Context& ctx) {
    if (source.size() > ctx.limits.maxContainerFileBytes) return ExtractStatus::TooLarge;
    PdfFile file(source, ctx);
    switch (file.open()) {
        case PdfFile::Open::Corrupt:
            ctx.result.detail = "damaged PDF: no readable page structure";
            return ExtractStatus::Corrupt;
        case PdfFile::Open::Encrypted:
            ctx.result.detail = "password-protected PDF";
            return ExtractStatus::Encrypted;
        case PdfFile::Open::Ok: break;
    }
    const Obj* root = file.val(file.trailer(), "Root");
    const Obj* pagesRoot = root != nullptr ? file.val(*root, "Pages") : nullptr;
    if (pagesRoot == nullptr) {
        ctx.result.detail = "damaged PDF: no page tree";
        return ExtractStatus::Corrupt;
    }

    struct Node { const Obj* node; const Obj* resources; };
    std::vector<Node> stack{{pagesRoot, nullptr}};
    std::set<const Obj*> seen;
    PageReader reader(file, ctx);
    std::size_t pageNumber = 0, imagePages = 0, unmappablePages = 0;
    bool anyText = false;
    while (!stack.empty() && !ctx.sink.full()) {
        if (ctx.budget.expired()) { ctx.result.detail = ctx.budget.cancelled() ? "cancelled" : "time limit reached"; break; }
        const Node current = stack.back();
        stack.pop_back();
        if (current.node == nullptr || current.node->type != Obj::Type::Dict || !seen.insert(current.node).second || seen.size() > 2000000) continue;
        const Obj* resources = file.val(*current.node, "Resources");
        if (resources == nullptr) resources = current.resources;
        const Obj* kids = file.val(*current.node, "Kids");
        const Obj* type = file.val(*current.node, "Type");
        if (kids != nullptr && kids->type == Obj::Type::Array && !(type != nullptr && type->isName("Page"))) {
            for (std::size_t i = kids->items.size(); i-- > 0;) stack.push_back({file.fetch(&kids->items[i]), resources});
            continue;
        }
        ++pageNumber;
        bool images = false, unmappable = false;
        std::string text = reader.readPage(*current.node, resources, images, unmappable);
        bool hasText = false;
        for (const char ch : text) {
            if (std::isalnum(static_cast<unsigned char>(ch)) || static_cast<unsigned char>(ch) >= 0x80) { hasText = true; break; }
        }
        if (hasText) {
            anyText = true;
            ctx.sink.newline();
            ctx.sink.segment("page " + std::to_string(pageNumber));
            ctx.sink.append(text);
            ctx.sink.newline();
        } else if (images) {
            ++imagePages;
        } else if (unmappable) {
            ++unmappablePages;
        }
    }
    ctx.result.units = pageNumber;
    if (pageNumber == 0) {
        ctx.result.detail = "damaged PDF: no pages";
        return ExtractStatus::Corrupt;
    }
    // Document information: title, author, subject, keywords.
    if (const Obj* info = file.val(file.trailer(), "Info"); info != nullptr && info->type == Obj::Type::Dict) {
        bool labelled = false;
        for (const char* key : {"Title", "Author", "Subject", "Keywords"}) {
            const Obj* v = file.val(*info, key);
            if (v == nullptr || v->type != Obj::Type::String) continue;
            if (!labelled) { ctx.sink.newline(); ctx.sink.segment("document properties"); labelled = true; }
            ctx.sink.append(pdfTextString(v->str));
            ctx.sink.newline();
        }
    }
    if (!anyText) {
        if (imagePages > 0) {
            ctx.result.detail = "scanned PDF: " + std::to_string(imagePages) + " page(s) are images without a text layer (OCR is not available)";
            return ExtractStatus::ImageOnly;
        }
        ctx.result.detail = unmappablePages > 0 ? "text uses fonts without a character map" : "no text on any page";
        return unmappablePages > 0 ? ExtractStatus::Unsupported : ExtractStatus::Empty;
    }
    if (imagePages > 0) ctx.result.detail = std::to_string(imagePages) + " of " + std::to_string(pageNumber) + " pages are images without text (scanned)";
    return ExtractStatus::Ok;
}

}  // namespace flux::extract_detail
