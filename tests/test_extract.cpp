// Content extraction: every supported format is read from a real file that contains a unique known word,
// and the word, its page/slide/sheet label and the failure statuses are checked. Fixtures: tests/fixtures
// (regenerate with tests/fixtures/make_fixtures.py).
#include "flux/extract.hpp"
#include "test_util.hpp"

#include <atomic>
#include <string>

namespace fs = std::filesystem;
using flux::ExtractStatus;

namespace {

fs::path fixture(const std::string& name) { return fs::path(FLUX_FIXTURES_DIR) / name; }

flux::ExtractResult read(const std::string& name, bool segments = true) {
    flux::ExtractLimits limits;
    limits.wantSegments = segments;
    return flux::extractContent(fixture(name), limits);
}

bool has(const flux::ExtractResult& r, const std::string& needle) { return r.text.find(needle) != std::string::npos; }

// The label of the segment that holds `word`.
std::string where(const flux::ExtractResult& r, const std::string& word) {
    const std::size_t at = r.text.find(word);
    return at == std::string::npos ? "(missing)" : flux::segmentLabelAt(r.segments, at);
}

void testPdfTextAndPageNumbers() {
    const auto r = read("sample.pdf");
    CHECK(r.status == ExtractStatus::Ok);
    CHECK(r.units == 3);
    CHECK(has(r, "flxpdfone") && has(r, "flxpdftwo") && has(r, "flxpdfthree"));
    CHECK(where(r, "flxpdfone") == "page 1");
    CHECK(where(r, "flxpdftwo") == "page 2");
    CHECK(where(r, "flxpdfthree") == "page 3");
    CHECK(has(r, "Faheem Nazir appears here"));
    CHECK(has(r, "second page"));          // words on one line are joined by spaces ...
    CHECK(!has(r, "secondpage"));
    CHECK(has(r, "flxpdftitle"));          // document information
    CHECK(where(r, "flxpdftitle") == "document properties");
}

void testPdfVariants() {
    const auto plain = read("sample_uncompressed.pdf");
    CHECK(plain.status == ExtractStatus::Ok && has(plain, "flxplainpdf page one"));

    // PDF 1.5: object streams, a cross-reference stream, and a Type0/Identity-H font that only a ToUnicode map explains.
    const auto cid = read("sample_objstm.pdf");
    CHECK(cid.status == ExtractStatus::Ok);
    CHECK(has(cid, "flxcidone hello world") && has(cid, "flxcidword second page"));
    CHECK(where(cid, "flxcidword") == "page 2");
}

// Browser-made PDFs place every glyph separately and use Type 3 fonts whose widths are scaled by the FontMatrix:
// the word must come out whole, not as "f l x t y p e ...".
void testPdfWithTypeThreeFontKeepsWordsWhole() {
    const auto r = read("type3.pdf");
    CHECK(r.status == ExtractStatus::Ok);
    CHECK(has(r, "flxtypethreeword"));
}

void testPdfFailuresAreStatusesNotCrashes() {
    CHECK(read("scanned.pdf").status == ExtractStatus::ImageOnly);   // pictures only: would need OCR
    CHECK(read("empty.pdf").status == ExtractStatus::Empty);
    const auto encrypted = read("encrypted.pdf");
    CHECK(encrypted.status == ExtractStatus::Encrypted && encrypted.text.empty());
    CHECK(read("garbage.pdf").status == ExtractStatus::Corrupt);
    const auto truncated = read("truncated.pdf");   // cut in the middle: whatever is readable, never a crash
    CHECK(truncated.status == ExtractStatus::Ok || truncated.status == ExtractStatus::Corrupt);
    CHECK(flux::extractContent(fixture("does_not_exist.pdf")).status == ExtractStatus::Unreadable);
}

void testDocx() {
    const auto r = read("sample.docx");
    CHECK(r.status == ExtractStatus::Ok);
    CHECK(has(r, "flxdocxheading") && has(r, "flxdocxbody") && has(r, "Faheem"));
    CHECK(has(r, "flxdocxtable") && has(r, "second cell"));        // table cells
    CHECK(has(r, "flxdocxheader") && has(r, "flxdocxfootnote"));   // header and footnotes
    CHECK(has(r, "flxdocxtitle"));                                 // core properties
    CHECK(!has(r, "flxdocxdeleted"));                              // tracked deletions are not text
    CHECK(where(r, "flxdocxbody") == "page 1");
    CHECK(where(r, "flxdocxpagetwo") == "page 2");
    CHECK(read("empty.docx").status == ExtractStatus::Empty);
}

void testPptxSlidesInPresentationOrderWithNotes() {
    const auto r = read("sample.pptx");
    CHECK(r.status == ExtractStatus::Ok && r.units == 3);
    CHECK(where(r, "flxslideone") == "slide 1");
    CHECK(where(r, "flxslidethree") == "slide 2");   // slide3.xml is second in presentation.xml
    CHECK(where(r, "flxslidetwo") == "slide 3");
    CHECK(where(r, "flxslidetable") == "slide 3");   // table cell
    CHECK(where(r, "flxslidenotes") == "slide 3 notes");
}

void testXlsxCellsSheetsAndFormulas() {
    const auto r = read("sample.xlsx");
    CHECK(r.status == ExtractStatus::Ok && r.units == 2);
    CHECK(where(r, "flxxlsxname") == "Students!A2");      // shared string
    CHECK(where(r, "flxxlsxinline") == "Students!B3");    // inline string
    CHECK(has(r, "Faheem Nazir") && where(r, "Faheem Nazir") == "Students!A3");  // rich text runs joined
    CHECK(has(r, "91") && has(r, "182"));                  // numbers and cached formula results
    CHECK(has(r, "=SUM(B2:B3)"));                          // a formula without a cached value keeps its formula
    CHECK(where(r, "flxxlsxtotals") == "Totals!A1");
}

void testOpenDocument() {
    const auto odt = read("sample.odt");
    CHECK(odt.status == ExtractStatus::Ok && has(odt, "flxodtheading") && has(odt, "flxodtbody") && has(odt, "flxodttitle"));
    const auto ods = read("sample.ods");
    CHECK(ods.status == ExtractStatus::Ok && has(ods, "flxodscell") && where(ods, "flxodscell") == "sheet Budget");
    const auto odp = read("sample.odp");
    CHECK(odp.status == ExtractStatus::Ok && where(odp, "flxodpslideone") == "slide 1" && where(odp, "flxodpslidetwo") == "slide 2");
}

void testRtfHtmlEmailEpubZip() {
    const auto rtf = read("sample.rtf");
    CHECK(rtf.status == ExtractStatus::Ok && has(rtf, "flxrtfword") && has(rtf, "caf\xC3\xA9") && has(rtf, "\xE2\x82\xAC"));
    CHECK(!has(rtf, "fontnamejunk"));  // the font table is not text

    const auto html = read("sample.html");
    CHECK(html.status == ExtractStatus::Ok);
    CHECK(has(html, "flxhtmltitle") && has(html, "flxhtmlheading") && has(html, "Paragraph with flxhtmlbody & Faheem"));
    CHECK(!has(html, "flxhtmlscript") && !has(html, "flxhtmlstyle") && !has(html, "flxhtmlcomment"));

    const auto eml = read("sample.eml");
    CHECK(eml.status == ExtractStatus::Ok);
    CHECK(has(eml, "flxemlsubject") && has(eml, "Faheem Nazir") && has(eml, "flxemlbody") && has(eml, "caf\xC3\xA9"));
    CHECK(has(eml, "soft break continues here"));  // quoted-printable soft line break
    CHECK(has(eml, "flxattach.txt") && has(eml, "attachment body"));   // a text attachment is part of the message

    const auto epub = read("sample.epub");
    CHECK(epub.status == ExtractStatus::Ok && has(epub, "flxepubone") && has(epub, "flxepubtwo"));
    CHECK(where(epub, "flxepubtwo") == "section 2");

    const auto zip = read("archive.zip");
    CHECK(zip.status == ExtractStatus::Ok);
    CHECK(has(zip, "inner.docx") && has(zip, "flxdocxbody") && has(zip, "flxziptext"));   // a document inside an archive
    CHECK(where(zip, "flxziptext") == "note.txt");
}

void testTextEncodings() {
    const auto utf16 = read("unicode_utf16.txt");
    CHECK(utf16.status == ExtractStatus::Ok && has(utf16, "flxutfsixteen") && has(utf16, "caf\xC3\xA9") && has(utf16, "\xE6\x97\xA5\xE6\x9C\xAC"));
    const auto utf8 = read("unicode_utf8.txt");
    CHECK(utf8.status == ExtractStatus::Ok && has(utf8, "flxutfeight") && has(utf8, "M\xC3\xBCller"));
    const auto latin1 = read("latin1.txt");   // not valid UTF-8: read as Windows-1252, stored as UTF-8
    CHECK(latin1.status == ExtractStatus::Ok && has(latin1, "flxlatin") && has(latin1, "caf\xC3\xA9"));
    CHECK(read("source.py").status == ExtractStatus::Ok && has(read("source.py"), "flxsourcefunc"));
    CHECK(has(read("data.csv"), "flxcsvname"));
}

void testPasswordProtectedAndDamagedOfficeFiles() {
    const auto locked = read("locked.docx");
    CHECK(locked.status == ExtractStatus::Encrypted && locked.text.empty());
    CHECK(std::string(flux::extractStatusName(locked.status)) == "password required");
    CHECK(read("corrupt.docx").status == ExtractStatus::Corrupt);
    CHECK(read("truncated.docx").status == ExtractStatus::Corrupt);
    CHECK(read("empty.docx").status != ExtractStatus::Corrupt);
}

void testFormatTable() {
    using flux::ContentKind;
    CHECK(flux::contentKindFor("a.PDF") == ContentKind::Pdf);
    CHECK(flux::contentKindFor("dir/b.docx") == ContentKind::Docx);
    CHECK(flux::contentKindFor("c.xlsx") == ContentKind::Xlsx && flux::contentKindFor("d.pptx") == ContentKind::Pptx);
    CHECK(flux::contentKindFor("e.odt") == ContentKind::Odf && flux::contentKindFor("f.csv") == ContentKind::PlainText);
    CHECK(flux::contentKindFor("g.tsx") == ContentKind::PlainText && flux::contentKindFor("h.yaml") == ContentKind::PlainText);
    CHECK(flux::contentKindFor("i.htm") == ContentKind::Html && flux::contentKindFor("j.eml") == ContentKind::Eml);
    CHECK(flux::contentKindFor("photo.jpg") == ContentKind::None && flux::contentKindFor("old.doc") == ContentKind::None);
    CHECK(flux::contentKindFor("noextension") == ContentKind::None);
    // Formats without a reader say why, and are never read as text.
    const auto doc = flux::extractContent(fixture("source.py").parent_path() / "old.doc");
    CHECK(doc.status == ExtractStatus::Unsupported);
    CHECK(flux::unsupportedReason("old.doc").find("binary") != std::string::npos);
    CHECK(flux::unsupportedReason("photo.png").find("OCR") != std::string::npos);
}

void testLimitsAndCancellation() {
    flux::ExtractLimits tiny;
    tiny.maxTextBytes = 64;
    const auto cut = flux::extractContent(fixture("sample.pdf"), tiny);
    CHECK(cut.status == ExtractStatus::Truncated && cut.text.size() <= 64 && !cut.text.empty());   // the start is still searchable

    flux::ExtractLimits small;
    small.maxStreamBytes = 100;   // a part that expands beyond this is refused, not read
    CHECK(flux::extractContent(fixture("sample.docx"), small).status != ExtractStatus::Ok);

    flux::ExtractLimits capped;
    capped.maxPlainTextFileBytes = 10;
    CHECK(flux::extractContent(fixture("source.py"), capped).status == ExtractStatus::TooLarge);

    std::atomic<bool> cancel{true};
    flux::ExtractLimits cancelled;
    cancelled.cancel = &cancel;
    CHECK(flux::extractContent(fixture("sample.pdf"), cancelled).status == ExtractStatus::Unreadable);
}

void testMemoryBuffers() {
    const std::string bytes = "plain flxmemoryword text";
    const auto r = flux::extractContentFromMemory("note.txt", bytes);
    CHECK(r.status == ExtractStatus::Ok && has(r, "flxmemoryword"));
    CHECK(flux::extractContentFromMemory("nothing.pdf", std::string("not a pdf")).status == ExtractStatus::Corrupt);
    CHECK(flux::extractContentFromMemory("x.docx", std::string()).status == ExtractStatus::Corrupt);
}

}  // namespace

int main() {
    RUN(testPdfTextAndPageNumbers);
    RUN(testPdfVariants);
    RUN(testPdfWithTypeThreeFontKeepsWordsWhole);
    RUN(testPdfFailuresAreStatusesNotCrashes);
    RUN(testDocx);
    RUN(testPptxSlidesInPresentationOrderWithNotes);
    RUN(testXlsxCellsSheetsAndFormulas);
    RUN(testOpenDocument);
    RUN(testRtfHtmlEmailEpubZip);
    RUN(testTextEncodings);
    RUN(testPasswordProtectedAndDamagedOfficeFiles);
    RUN(testFormatTable);
    RUN(testLimitsAndCancellation);
    RUN(testMemoryBuffers);
    return testExitCode();
}
