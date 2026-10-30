// Filename + content search (searchFiles / addSnippets). Included by test_search.cpp.

namespace {

flux::ProcessedDocument namedDoc(const std::string& path, std::vector<std::pair<std::string, std::uint32_t>> terms,
                                 std::uint64_t size = 100) {
    flux::ProcessedDocument d = makeDoc(path, std::move(terms));
    d.sizeBytes = size;
    return d;
}

// Documents with realistic names; /docs/tax_return_2023.txt also mentions "invoice".
void fillNames(flux::Database& db) {
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::IndexWriter writer(db);
    writer.submit(namedDoc("/docs/tax_return_2023.txt", {{"invoice", 2}, {"total", 1}}));
    writer.submit(namedDoc("/docs/Report Final.PDF", {}));            // name only: never read
    writer.submit(namedDoc("/docs/report.txt", {{"report", 3}}));      // name AND content
    writer.submit(namedDoc("/other/notes.md", {{"invoice", 5}}));      // content only
    writer.submit(namedDoc("/other/quarterly-report-2024.docx", {}));
    writer.submit(namedDoc("/other/\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.pdf", {}));  // UTF-8 name
    CHECK(writer.finish(error));
}

std::vector<std::string> names(const std::vector<flux::FileHit>& hits) {
    std::vector<std::string> out;
    for (const auto& h : hits) out.push_back(h.path);
    return out;
}

bool runSearch(flux::Database& db, const std::string& query, std::vector<flux::FileHit>& hits, std::size_t limit = 50) {
    std::string error;
    const bool ok = flux::searchFiles(db, query, limit, hits, error);
    if (!ok) std::cerr << "search failed: " << error << '\n';
    return ok;
}

}  // namespace

void testFilenameSearchExactPartialAndWithoutExtension() {
    flux::Database db;
    fillNames(db);
    std::vector<flux::FileHit> hits;
    CHECK(runSearch(db, "notes.md", hits));  // exact filename
    CHECK(names(hits) == (Paths{"/other/notes.md"}) && hits[0].nameRank == 4);
    CHECK(runSearch(db, "notes", hits));  // filename without extension
    CHECK(names(hits) == (Paths{"/other/notes.md"}) && hits[0].nameRank == 4);
    CHECK(runSearch(db, "tax_ret", hits));  // partial filename
    CHECK(names(hits) == (Paths{"/docs/tax_return_2023.txt"}));
    CHECK(runSearch(db, "quarterly", hits));
    CHECK(names(hits) == (Paths{"/other/quarterly-report-2024.docx"}));
}

void testFilenameSearchIsCaseInsensitiveAndIgnoresPunctuation() {
    flux::Database db;
    fillNames(db);
    std::vector<flux::FileHit> hits;
    CHECK(runSearch(db, "REPORT FINAL.pdf", hits));
    CHECK(names(hits) == (Paths{"/docs/Report Final.PDF"}) && hits[0].nameRank == 4);
    CHECK(runSearch(db, "  Tax,  RETURN! ", hits));  // extra spaces and punctuation
    CHECK(names(hits) == (Paths{"/docs/tax_return_2023.txt"}));
    CHECK(runSearch(db, "tax-return", hits));  // a hyphen in the query, an underscore in the name
    CHECK(names(hits) == (Paths{"/docs/tax_return_2023.txt"}));
}

void testMultipleWordsMustAllBeInTheName() {
    flux::Database db;
    fillNames(db);
    std::vector<flux::FileHit> hits;
    CHECK(runSearch(db, "tax 2023", hits));
    CHECK(names(hits) == (Paths{"/docs/tax_return_2023.txt"}));
    CHECK(runSearch(db, "report 2024", hits));
    CHECK(names(hits) == (Paths{"/other/quarterly-report-2024.docx"}));
    CHECK(runSearch(db, "tax 2024", hits));  // no file has both
    CHECK(hits.empty());
}

void testFolderNamesAreNotFilenameMatches() {
    flux::Database db;
    fillNames(db);
    std::vector<flux::FileHit> hits;
    CHECK(runSearch(db, "docs", hits));  // a folder name only: not a file name, and no file contains the word
    CHECK(hits.empty());
}

void testNameAndContentMatchesAreCombinedAndRanked() {
    flux::Database db;
    fillNames(db);
    std::vector<flux::FileHit> hits;
    CHECK(runSearch(db, "report", hits));
    CHECK(hits.size() == 3);
    CHECK(hits[0].path == "/docs/report.txt" && hits[0].kind == flux::MatchKind::Both && hits[0].contentScore == 3 &&
          hits[0].nameRank == 4);  // exact stem, and the word is inside the text
    CHECK(hits[1].kind == flux::MatchKind::Filename && hits[1].contentScore == 0);
    CHECK(hits[2].kind == flux::MatchKind::Filename);

    CHECK(runSearch(db, "invoice", hits));  // content in two files, no file is named invoice
    CHECK(names(hits) == (Paths{"/other/notes.md", "/docs/tax_return_2023.txt"}));  // score 5 then 2
    CHECK(hits[0].kind == flux::MatchKind::Content && hits[0].nameRank == 0);
}

void testSearchFilesKeepsTheContentRulesOfSearchIndex() {
    flux::Database db;
    fill(db);  // /a cat3 dog1, /b cat1, /c dog2 bird1, /d cat3 dog1
    std::vector<flux::FileHit> hits;
    CHECK(runSearch(db, "cat dog", hits));
    CHECK(names(hits) == (Paths{"/a", "/d"}) && hits[0].contentScore == 4);
    CHECK(runSearch(db, "CAT, Dog!", hits));
    CHECK(hits.size() == 2);
    CHECK(runSearch(db, "unicorn", hits) && hits.empty());
    std::string error;
    CHECK(!flux::searchFiles(db, " ,.; ", 10, hits, error) && error == "query has no searchable terms");
    CHECK(!flux::searchFiles(db, "", 10, hits, error));
    CHECK(runSearch(db, "cat", hits, 2) && hits.size() == 2);  // limit
}

void testNonAsciiFilenamesAreFound() {
    flux::Database db;
    fillNames(db);
    std::vector<flux::FileHit> hits;
    CHECK(runSearch(db, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", hits));  // the query has no ASCII word at all
    CHECK(names(hits) == (Paths{"/other/\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.pdf"}));
}

void testMetadataAndDisplayHelpers() {
    flux::Database db;
    fillNames(db);
    std::vector<flux::FileHit> hits;
    CHECK(runSearch(db, "notes.md", hits) && hits.size() == 1);
    CHECK(hits[0].sizeBytes == 100);
    CHECK(flux::fileTypeLabel("/x/archive.tar.GZ") == "GZ file" && flux::fileTypeLabel("/x/Makefile") == "File" &&
          flux::fileTypeLabel("/x/.gitignore") == "File" && flux::fileTypeLabel("/x/dir.v2/readme") == "File");
    CHECK(flux::formatFileSize(0) == "0 bytes" && flux::formatFileSize(1536) == "1.5 KiB" &&
          flux::formatFileSize(5u << 20) == "5.0 MiB");
    CHECK(flux::formatLocalDateTime(0).empty() && flux::formatLocalDateTime(86400 * 365).size() == 16);
}

void testSameNameInDifferentFoldersAndUnknownRoot() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::IndexWriter writer(db);
    writer.submit(namedDoc("/one/same.txt", {{"hello", 1}}));
    writer.submit(namedDoc("/two/same.txt", {{"hello", 1}}));
    CHECK(writer.finish(error));
    std::vector<flux::FileHit> hits;
    CHECK(runSearch(db, "same", hits) && hits.size() == 2);  // duplicate names in different folders both appear
    CHECK(flux::searchFiles(db, "same", 10, hits, error, 12345) && hits.empty());  // unknown root id: nothing
}

void testSnippetsComeFromTheFileAsItIsNow() {
    TempDir dir("search_snippets");
    dir.write("long.txt", std::string(200, 'x') + "\n  The   Invoice\ttotal is due\n" + std::string(200, 'y'));
    dir.write("short.txt", "invoice");
    dir.write("utf.txt", "caf\xC3\xA9 caf\xC3\xA9 invoice caf\xC3\xA9");
    dir.write("binary.bin", std::string("invoice\0data", 12));
    std::vector<flux::FileHit> hits(5);
    const char* files[] = {"long.txt", "short.txt", "utf.txt", "binary.bin", "gone.txt"};
    for (int i = 0; i < 5; ++i) {
        hits[static_cast<size_t>(i)].path = flux::normalizedPath(dir.path() / files[i]);
        hits[static_cast<size_t>(i)].contentScore = 1;
    }
    hits.push_back({});
    hits.back().path = flux::normalizedPath(dir.path() / "short.txt");  // a name-only hit: never read
    flux::addSnippets(hits, "INVOICE total");
    CHECK(hits[0].snippet.find("The Invoice total is due") != std::string::npos);  // whitespace collapsed
    CHECK(hits[0].snippet.rfind("...", 0) == 0 && hits[0].snippet.size() < 130);   // trimmed on both sides
    CHECK(hits[1].snippet == "invoice");
    CHECK(hits[2].snippet.find("invoice") != std::string::npos && hits[2].snippet.find("caf\xC3\xA9") != std::string::npos);
    CHECK(hits[3].snippet.empty());  // binary content is not shown
    CHECK(hits[4].snippet.empty());  // a vanished file is not an error
    CHECK(hits[5].snippet.empty());  // a hit without a content match is never read
}
