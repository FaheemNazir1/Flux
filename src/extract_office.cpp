// Office Open XML (DOCX, PPTX, XLSX), OpenDocument (ODT, ODP, ODS), EPUB and ZIP archives. These are ZIP
// files of XML; only the text-carrying XML parts are read. Macros (vbaProject.bin), embedded objects,
// external links and scripts are never opened, let alone run.
#include "extract_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace flux::extract_detail {
namespace {

const unsigned char kOleMagic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

// Opens the ZIP package of an Office-style file, or sets the status explaining why not.
bool openPackage(const ByteSource& source, ZipArchive& zip, Context& ctx, ExtractStatus& status) {
    if (source.size() > ctx.limits.maxContainerFileBytes) { status = ExtractStatus::TooLarge; return false; }
    unsigned char magic[8] = {};
    if (source.size() >= 8 && source.read(0, magic, 8) && std::memcmp(magic, kOleMagic, 8) == 0) {
        // A password-protected OOXML file is stored as an OLE container holding an EncryptedPackage stream.
        ctx.result.detail = "password-protected Office file";
        status = ExtractStatus::Encrypted;
        return false;
    }
    std::string error;
    if (!zip.open(source, error)) {
        ctx.result.detail = error;
        status = ExtractStatus::Corrupt;
        return false;
    }
    return true;
}

// Reads one package part. Returns false and sets `status` only for conditions that make the whole file
// unreadable (encrypted); a damaged or missing part yields an empty string and `true`.
enum class Part { Ok, Missing, Bad };
Part readPart(const ZipArchive& zip, std::string_view name, std::string& out, Context& ctx, ExtractStatus& status) {
    const ZipEntry* entry = zip.find(name);
    if (entry == nullptr) { out.clear(); return Part::Missing; }
    const ZipRead r = zip.read(*entry, out, ctx.limits.maxStreamBytes);
    if (r == ZipRead::Ok) return Part::Ok;
    if (r == ZipRead::Encrypted) { status = ExtractStatus::Encrypted; ctx.result.detail = "encrypted ZIP entry"; }
    out.clear();
    return Part::Bad;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool endsWith(const std::string& text, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

// Natural order of "slide2.xml" < "slide10.xml".
bool naturalLess(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            std::size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            const unsigned long long x = std::strtoull(a.substr(i, ie - i).c_str(), nullptr, 10);
            const unsigned long long y = std::strtoull(b.substr(j, je - j).c_str(), nullptr, 10);
            if (x != y) return x < y;
            i = ie; j = je;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i; ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

// Core document properties (title, subject, author, keywords, ...) of an OOXML or ODF package.
void appendProperties(const ZipArchive& zip, const char* part, Context& ctx, ExtractStatus& status) {
    std::string xml;
    if (readPart(zip, part, xml, ctx, status) != Part::Ok) return;
    XmlScanner scanner(xml);
    XmlEvent event;
    bool in = false;
    bool labelled = false;
    while (scanner.next(event) && !ctx.sink.full()) {
        if (event.kind == XmlEvent::Kind::Start) {
            const std::string_view local = localName(event.name);
            in = local == "title" || local == "subject" || local == "creator" || local == "keywords" || local == "keyword" ||
                 local == "description" || local == "lastModifiedBy" || local == "initial-creator";
        } else if (event.kind == XmlEvent::Kind::Close) {
            if (in) ctx.sink.newline();
            in = false;
        } else if (event.kind == XmlEvent::Kind::Text && in) {
            if (!labelled) { ctx.sink.newline(); ctx.sink.segment("document properties"); labelled = true; }
            ctx.sink.append(event.text);
        }
    }
}

// ------------------------------------------------------------------------------------------------ DOCX

void docxPart(std::string_view xml, Context& ctx, const std::string& label, bool paged, int& page, bool& pageLabelled) {
    if (!label.empty()) { ctx.sink.newline(); ctx.sink.segment(label); }
    const bool useRendered = xml.find("lastRenderedPageBreak") != std::string_view::npos;
    const std::uint64_t bodyStart = ctx.sink.size();
    XmlScanner scanner(xml);
    XmlEvent event;
    bool inText = false;
    auto startPage = [&] {
        if (!paged) return;
        if (!pageLabelled) { ctx.sink.segmentAt(bodyStart, "page 1"); pageLabelled = true; }
        ++page;
        ctx.sink.segment("page " + std::to_string(page));
    };
    while (!ctx.sink.full() && scanner.next(event)) {
        if (ctx.budget.expired()) break;
        const std::string_view name = localName(event.name);
        switch (event.kind) {
            case XmlEvent::Kind::Start:
                if (name == "t") inText = !event.selfClosing;
                else if (name == "tab") ctx.sink.space();
                else if (name == "br" || name == "cr") {
                    if (xmlAttr(event.attrs, "w:type") == "page") { if (!useRendered) startPage(); }
                    ctx.sink.newline();
                } else if (name == "lastRenderedPageBreak") { startPage(); }
                break;
            case XmlEvent::Kind::Close:
                if (name == "t") inText = false;
                else if (name == "p") ctx.sink.newline();
                else if (name == "tc") ctx.sink.space();
                break;
            case XmlEvent::Kind::Text:
                if (inText) ctx.sink.append(event.text);
                break;
            case XmlEvent::Kind::End: break;
        }
    }
}

// ------------------------------------------------------------------------------------------------ PPTX

// Text of a slide or notes page; paragraphs become lines.
void drawingText(std::string_view xml, Context& ctx) {
    XmlScanner scanner(xml);
    XmlEvent event;
    bool inText = false;
    while (!ctx.sink.full() && scanner.next(event)) {
        if (ctx.budget.expired()) break;
        const std::string_view name = localName(event.name);
        if (event.kind == XmlEvent::Kind::Start) {
            if (name == "t") inText = !event.selfClosing;
            else if (name == "br") ctx.sink.newline();
        } else if (event.kind == XmlEvent::Kind::Close) {
            if (name == "t") inText = false;
            else if (name == "p") ctx.sink.newline();
            else if (name == "tc") ctx.sink.space();
        } else if (event.kind == XmlEvent::Kind::Text && inText) {
            ctx.sink.append(event.text);
        }
    }
}

// Relationship targets of a .rels part: Id -> Target.
std::map<std::string, std::string> readRelationships(const ZipArchive& zip, const std::string& part, Context& ctx, ExtractStatus& status) {
    std::map<std::string, std::string> rels;
    std::string xml;
    if (readPart(zip, part, xml, ctx, status) != Part::Ok) return rels;
    XmlScanner scanner(xml);
    XmlEvent event;
    while (scanner.next(event)) {
        if (event.kind == XmlEvent::Kind::Start && localName(event.name) == "Relationship") {
            rels[xmlAttr(event.attrs, "Id")] = xmlAttr(event.attrs, "Target");
        }
    }
    return rels;
}

// Resolves a relationship target against the directory of the part that owns it ("../notesSlides/n1.xml").
std::string resolveTarget(std::string baseDir, const std::string& target) {
    if (!target.empty() && target[0] == '/') return target.substr(1);
    std::string path = baseDir + target;
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::string piece = path.substr(start, end - start);
        if (piece == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!piece.empty() && piece != ".") parts.push_back(piece);
        start = end + 1;
    }
    std::string result;
    for (const std::string& p : parts) result += (result.empty() ? "" : "/") + p;
    return result;
}

}  // namespace

ExtractStatus extractDocx(const ByteSource& source, Context& ctx) {
    ZipArchive zip;
    ExtractStatus status = ExtractStatus::Corrupt;
    if (!openPackage(source, zip, ctx, status)) return status;
    status = ExtractStatus::Ok;
    std::string xml;
    if (readPart(zip, "word/document.xml", xml, ctx, status) != Part::Ok) {
        if (status == ExtractStatus::Encrypted) return status;
        ctx.result.detail = "no readable word/document.xml (not a Word document)";
        return ExtractStatus::Corrupt;
    }
    int page = 1;
    bool pageLabelled = false;
    docxPart(xml, ctx, "", true, page, pageLabelled);
    ctx.result.units = static_cast<std::size_t>(page);

    // Footnotes, endnotes, comments, headers and footers: text that belongs to the document too.
    std::vector<std::string> extra;
    for (const ZipEntry& entry : zip.entries()) {
        const std::string name = lower(entry.name);
        if (name == "word/footnotes.xml" || name == "word/endnotes.xml" || name == "word/comments.xml" ||
            (name.rfind("word/header", 0) == 0 && endsWith(name, ".xml")) || (name.rfind("word/footer", 0) == 0 && endsWith(name, ".xml"))) {
            extra.push_back(entry.name);
        }
    }
    std::sort(extra.begin(), extra.end(), naturalLess);
    for (const std::string& part : extra) {
        if (ctx.sink.full() || ctx.budget.expired()) break;
        if (readPart(zip, part, xml, ctx, status) != Part::Ok) continue;
        std::string label = part.substr(5);
        label = label.substr(0, label.find('.'));
        int ignoredPage = 0;
        bool ignoredLabel = true;
        docxPart(xml, ctx, label, false, ignoredPage, ignoredLabel);
    }
    appendProperties(zip, "docProps/core.xml", ctx, status);
    return ExtractStatus::Ok;
}

ExtractStatus extractPptx(const ByteSource& source, Context& ctx) {
    ZipArchive zip;
    ExtractStatus status = ExtractStatus::Corrupt;
    if (!openPackage(source, zip, ctx, status)) return status;
    status = ExtractStatus::Ok;

    // Slide order: the order of <p:sldId> in presentation.xml, resolved through its relationships.
    std::vector<std::string> slides;
    {
        std::string presentation;
        const auto rels = readRelationships(zip, "ppt/_rels/presentation.xml.rels", ctx, status);
        if (readPart(zip, "ppt/presentation.xml", presentation, ctx, status) == Part::Ok) {
            XmlScanner scanner(presentation);
            XmlEvent event;
            while (scanner.next(event)) {
                if (event.kind == XmlEvent::Kind::Start && localName(event.name) == "sldId") {
                    const auto it = rels.find(xmlAttr(event.attrs, "r:id"));
                    if (it != rels.end()) {
                        const std::string path = resolveTarget("ppt/", it->second);
                        if (zip.find(path) != nullptr) slides.push_back(path);
                    }
                }
            }
        }
    }
    if (status == ExtractStatus::Encrypted) return status;
    if (slides.empty()) {  // fall back to the numeric order of the slide parts
        for (const ZipEntry& entry : zip.entries()) {
            if (entry.name.rfind("ppt/slides/slide", 0) == 0 && endsWith(entry.name, ".xml")) slides.push_back(entry.name);
        }
        std::sort(slides.begin(), slides.end(), naturalLess);
    }
    if (slides.empty()) {
        ctx.result.detail = "no slides found (not a PowerPoint presentation)";
        return ExtractStatus::Corrupt;
    }

    std::string xml;
    std::size_t number = 0;
    for (const std::string& slide : slides) {
        if (ctx.sink.full() || ctx.budget.expired()) break;
        ++number;
        ctx.sink.newline();
        ctx.sink.segment("slide " + std::to_string(number));
        if (readPart(zip, slide, xml, ctx, status) == Part::Ok) drawingText(xml, ctx);
        // Speaker notes: the slide's own relationship to a notesSlide part.
        const std::size_t slash = slide.rfind('/');
        const std::string dir = slide.substr(0, slash + 1);
        const auto rels = readRelationships(zip, dir + "_rels/" + slide.substr(slash + 1) + ".rels", ctx, status);
        for (const auto& rel : rels) {
            if (rel.second.find("notesSlide") == std::string::npos) continue;
            const std::string notes = resolveTarget(dir, rel.second);
            if (readPart(zip, notes, xml, ctx, status) == Part::Ok) {
                ctx.sink.newline();
                ctx.sink.segment("slide " + std::to_string(number) + " notes");
                drawingText(xml, ctx);
            }
        }
    }
    ctx.result.units = number;
    appendProperties(zip, "docProps/core.xml", ctx, status);
    return ExtractStatus::Ok;
}

// ------------------------------------------------------------------------------------------------ XLSX

namespace {

std::vector<std::string> readSharedStrings(const ZipArchive& zip, Context& ctx, ExtractStatus& status) {
    std::vector<std::string> strings;
    std::string xml;
    if (readPart(zip, "xl/sharedStrings.xml", xml, ctx, status) != Part::Ok) return strings;
    XmlScanner scanner(xml);
    XmlEvent event;
    std::string current;
    bool inItem = false, inText = false, inPhonetic = false;
    while (scanner.next(event)) {
        if (ctx.budget.expired()) break;
        const std::string_view name = localName(event.name);
        if (event.kind == XmlEvent::Kind::Start) {
            if (name == "si") { inItem = true; current.clear(); }
            else if (name == "rPh") inPhonetic = true;
            else if (name == "t" && inItem && !inPhonetic) inText = !event.selfClosing;
        } else if (event.kind == XmlEvent::Kind::Close) {
            if (name == "si") { strings.push_back(std::move(current)); current.clear(); inItem = false; }
            else if (name == "rPh") inPhonetic = false;
            else if (name == "t") inText = false;
        } else if (event.kind == XmlEvent::Kind::Text && inText) {
            current += event.text;
        }
    }
    return strings;
}

}  // namespace

ExtractStatus extractXlsx(const ByteSource& source, Context& ctx) {
    ZipArchive zip;
    ExtractStatus status = ExtractStatus::Corrupt;
    if (!openPackage(source, zip, ctx, status)) return status;
    status = ExtractStatus::Ok;

    struct Sheet { std::string name, path; };
    std::vector<Sheet> sheets;
    {
        std::string workbook;
        const auto rels = readRelationships(zip, "xl/_rels/workbook.xml.rels", ctx, status);
        if (readPart(zip, "xl/workbook.xml", workbook, ctx, status) == Part::Ok) {
            XmlScanner scanner(workbook);
            XmlEvent event;
            while (scanner.next(event)) {
                if (event.kind == XmlEvent::Kind::Start && localName(event.name) == "sheet") {
                    const auto it = rels.find(xmlAttr(event.attrs, "r:id"));
                    if (it != rels.end()) sheets.push_back({xmlAttr(event.attrs, "name"), resolveTarget("xl/", it->second)});
                }
            }
        }
    }
    if (status == ExtractStatus::Encrypted) return status;
    if (sheets.empty()) {
        for (const ZipEntry& entry : zip.entries()) {
            if (entry.name.rfind("xl/worksheets/sheet", 0) == 0 && endsWith(entry.name, ".xml")) {
                sheets.push_back({"Sheet" + std::to_string(sheets.size() + 1), entry.name});
            }
        }
        std::sort(sheets.begin(), sheets.end(), [](const Sheet& a, const Sheet& b) { return naturalLess(a.path, b.path); });
    }
    if (sheets.empty()) {
        ctx.result.detail = "no worksheets found (not an Excel workbook)";
        return ExtractStatus::Corrupt;
    }

    const std::vector<std::string> shared = readSharedStrings(zip, ctx, status);
    std::string xml;
    for (const Sheet& sheet : sheets) {
        if (ctx.sink.full() || ctx.budget.expired()) break;
        if (readPart(zip, sheet.path, xml, ctx, status) != Part::Ok) continue;
        ++ctx.result.units;
        ctx.sink.newline();
        ctx.sink.segment("sheet " + sheet.name);
        XmlScanner scanner(xml);
        XmlEvent event;
        std::string ref, type, value, formula, inlineText;
        bool inCell = false, inV = false, inF = false, inInlineText = false;
        while (!ctx.sink.full() && scanner.next(event)) {
            if (ctx.budget.expired()) break;
            const std::string_view name = localName(event.name);
            if (event.kind == XmlEvent::Kind::Start) {
                if (name == "c") {
                    inCell = !event.selfClosing;
                    ref = xmlAttr(event.attrs, "r");
                    type = xmlAttr(event.attrs, "t");
                    value.clear(); formula.clear(); inlineText.clear();
                } else if (inCell && name == "v") inV = !event.selfClosing;
                else if (inCell && name == "f") inF = !event.selfClosing;
                else if (inCell && name == "t") inInlineText = !event.selfClosing;
            } else if (event.kind == XmlEvent::Kind::Close) {
                if (name == "v") inV = false;
                else if (name == "f") inF = false;
                else if (name == "t") inInlineText = false;
                else if (name == "c" && inCell) {
                    inCell = false;
                    std::string text;
                    if (type == "s") {
                        const std::size_t index = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
                        if (!value.empty() && index < shared.size()) text = shared[index];
                    } else if (type == "inlineStr") text = inlineText;
                    else if (type == "str") text = value;
                    else if (type == "b") text = value == "1" ? "TRUE" : "FALSE";
                    else if (type == "e") text.clear();
                    else text = value;
                    if (text.empty() && !formula.empty() && type != "e") text = "=" + formula;  // no cached result: keep the formula
                    if (!text.empty()) {
                        ctx.sink.segment(sheet.name + "!" + ref);
                        ctx.sink.append(text);
                        ctx.sink.append('\t');
                    }
                } else if (name == "row") ctx.sink.newline();
            } else if (event.kind == XmlEvent::Kind::Text) {
                if (inV) value += event.text;
                else if (inF) formula += event.text;
                else if (inInlineText) inlineText += event.text;
            }
        }
    }
    appendProperties(zip, "docProps/core.xml", ctx, status);
    return ExtractStatus::Ok;
}

// ------------------------------------------------------------------------------------------------ ODF

ExtractStatus extractOdf(const ByteSource& source, Context& ctx) {
    ZipArchive zip;
    ExtractStatus status = ExtractStatus::Corrupt;
    if (!openPackage(source, zip, ctx, status)) return status;
    status = ExtractStatus::Ok;
    std::string manifest;
    const ZipEntry* contentEntry = zip.find("content.xml");
    if (contentEntry == nullptr) {
        ctx.result.detail = "no content.xml (not an OpenDocument file)";
        return ExtractStatus::Corrupt;
    }
    // Encrypted ODF files declare it in the manifest; their content.xml is not readable text.
    if (readPart(zip, "META-INF/manifest.xml", manifest, ctx, status) == Part::Ok && manifest.find("encryption-data") != std::string::npos) {
        ctx.result.detail = "password-protected OpenDocument file";
        return ExtractStatus::Encrypted;
    }
    std::string xml;
    if (readPart(zip, "content.xml", xml, ctx, status) != Part::Ok) {
        if (status == ExtractStatus::Encrypted) return status;
        ctx.result.detail = "content.xml is unreadable";
        return ExtractStatus::Corrupt;
    }
    enum class Mode { Text, Presentation, Spreadsheet } mode = Mode::Text;
    XmlScanner scanner(xml);
    XmlEvent event;
    int slide = 0;
    int depthSkip = 0;  // inside office:automatic-styles / font-face-decls, which carry no text
    while (!ctx.sink.full() && scanner.next(event)) {
        if (ctx.budget.expired()) break;
        const std::string_view name = event.name;
        if (event.kind == XmlEvent::Kind::Start) {
            if (name == "office:presentation") mode = Mode::Presentation;
            else if (name == "office:spreadsheet") mode = Mode::Spreadsheet;
            else if (name == "office:automatic-styles" || name == "office:font-face-decls" || name == "office:scripts") depthSkip = event.selfClosing ? 0 : 1;
            else if (name == "draw:page" && mode == Mode::Presentation) {
                ++slide;
                ctx.sink.newline();
                ctx.sink.segment("slide " + std::to_string(slide));
            } else if (name == "table:table" && mode == Mode::Spreadsheet) {
                ++ctx.result.units;
                ctx.sink.newline();
                ctx.sink.segment("sheet " + xmlAttr(event.attrs, "table:name"));
            } else if (name == "text:tab" || name == "text:s") ctx.sink.space();
            else if (name == "text:line-break") ctx.sink.newline();
        } else if (event.kind == XmlEvent::Kind::Close) {
            if (name == "office:automatic-styles" || name == "office:font-face-decls" || name == "office:scripts") depthSkip = 0;
            else if (name == "text:p" || name == "text:h" || name == "table:table-row") ctx.sink.newline();
            else if (name == "table:table-cell" || name == "table:covered-table-cell") ctx.sink.space();
        } else if (event.kind == XmlEvent::Kind::Text && depthSkip == 0) {
            // Whitespace between elements is formatting, not text.
            if (event.text.find_first_not_of(" \t\r\n") != std::string::npos) ctx.sink.append(event.text);
        }
    }
    if (mode == Mode::Presentation) ctx.result.units = static_cast<std::size_t>(slide);
    appendProperties(zip, "meta.xml", ctx, status);
    return ExtractStatus::Ok;
}

// ----------------------------------------------------------------------------------------------- EPUB

ExtractStatus extractEpub(const ByteSource& source, Context& ctx) {
    ZipArchive zip;
    ExtractStatus status = ExtractStatus::Corrupt;
    if (!openPackage(source, zip, ctx, status)) return status;
    status = ExtractStatus::Ok;
    std::vector<std::string> chapters;
    for (const ZipEntry& entry : zip.entries()) {
        const std::string name = lower(entry.name);
        if (endsWith(name, ".xhtml") || endsWith(name, ".html") || endsWith(name, ".htm")) chapters.push_back(entry.name);
    }
    if (chapters.empty()) {
        ctx.result.detail = "no readable chapters (DRM-protected or not an EPUB)";
        return ExtractStatus::Corrupt;
    }
    std::sort(chapters.begin(), chapters.end(), naturalLess);
    std::string xml;
    for (const std::string& chapter : chapters) {
        if (ctx.sink.full() || ctx.budget.expired()) break;
        if (readPart(zip, chapter, xml, ctx, status) != Part::Ok) continue;
        ++ctx.result.units;
        ctx.sink.newline();
        ctx.sink.segment("section " + std::to_string(ctx.result.units));
        htmlToText(xml, ctx.sink, ctx.budget);
    }
    return ExtractStatus::Ok;
}

// ------------------------------------------------------------------------------------------------- ZIP

ExtractStatus extractZip(const ByteSource& source, Context& ctx, int depth) {
    ZipArchive zip;
    std::string error;
    if (source.size() > ctx.limits.maxContainerFileBytes) return ExtractStatus::TooLarge;
    if (!zip.open(source, error)) {
        ctx.result.detail = error;
        return ExtractStatus::Corrupt;
    }
    constexpr std::size_t kMaxEntries = 5000;
    constexpr std::size_t kMaxEntryBytes = 64 * 1024 * 1024;
    std::uint64_t budgetBytes = 256ull * 1024 * 1024;  // total decompressed bytes read from one archive (zip-bomb guard)
    std::size_t examined = 0, readable = 0;
    for (const ZipEntry& entry : zip.entries()) {
        if (ctx.sink.full() || ctx.budget.expired() || ++examined > kMaxEntries) break;
        if (entry.isDirectory()) continue;
        ctx.sink.newline();
        ctx.sink.segment(entry.name);
        ctx.sink.append(entry.name);  // the entry's name is searchable even when its content is not
        ctx.sink.newline();
        ContentKind kind = contentKindFor(entry.name);
        if (kind == ContentKind::None || (kind == ContentKind::Zip && depth >= 2)) continue;
        if (entry.size > kMaxEntryBytes || entry.size > budgetBytes) continue;
        std::string bytes;
        if (zip.read(entry, bytes, kMaxEntryBytes) != ZipRead::Ok) continue;
        budgetBytes -= std::min<std::uint64_t>(budgetBytes, bytes.size());
        MemorySource memory(bytes);
        ExtractResult nested;
        Context inner{ctx.limits, ctx.budget, ctx.sink, nested};
        if (dispatchExtract(kind, memory, inner, depth + 1) == ExtractStatus::Ok) ++readable;
        ctx.sink.newline();
    }
    ctx.result.units = readable;
    return ExtractStatus::Ok;
}

}  // namespace flux::extract_detail
