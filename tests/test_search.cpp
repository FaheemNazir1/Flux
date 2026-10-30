#include "flux/index_writer.hpp"
#include "flux/path_utils.hpp"
#include "flux/search.hpp"
#include "test_util.hpp"

namespace {

flux::ProcessedDocument makeDoc(const std::string& path, std::vector<std::pair<std::string, std::uint32_t>> terms) {
    flux::ProcessedDocument d;
    d.path = path;
    for (const auto& t : terms) d.tokenCount += t.second;
    d.termFrequencies = std::move(terms);
    return d;
}

// /a: cat x3, dog x1   /b: cat x1   /c: dog x2, bird x1   /d: cat x3, dog x1 (ties with /a)
void fill(flux::Database& db) {
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    flux::IndexWriter writer(db);
    writer.submit(makeDoc("/a", {{"cat", 3}, {"dog", 1}}));
    writer.submit(makeDoc("/b", {{"cat", 1}}));
    writer.submit(makeDoc("/c", {{"dog", 2}, {"bird", 1}}));
    writer.submit(makeDoc("/d", {{"cat", 3}, {"dog", 1}}));
    CHECK(writer.finish(error));
}

std::vector<std::string> paths(const std::vector<flux::SearchHit>& hits) {
    std::vector<std::string> out;
    for (const auto& h : hits) out.push_back(h.path);
    return out;
}

using Paths = std::vector<std::string>;

}  // namespace

void testSingleTermRankedByFrequencyThenPath() {
    flux::Database db;
    fill(db);
    std::vector<flux::SearchHit> hits;
    std::string error;
    CHECK(flux::searchIndex(db, "cat", 10, hits, error));
    CHECK(paths(hits) == (Paths{"/a", "/d", "/b"}));  // 3, 3 (path tiebreak), 1
    CHECK(hits[0].score == 3 && hits[2].score == 1);
}

void testMultiTermIsAnd() {
    flux::Database db;
    fill(db);
    std::vector<flux::SearchHit> hits;
    std::string error;
    CHECK(flux::searchIndex(db, "cat dog", 10, hits, error));
    CHECK(paths(hits) == (Paths{"/a", "/d"}));  // /b lacks dog, /c lacks cat
    CHECK(hits[0].score == 4);
    CHECK(flux::searchIndex(db, "dog bird", 10, hits, error));
    CHECK(paths(hits) == Paths{"/c"});
}

void testCaseInsensitiveAndPunctuation() {
    flux::Database db;
    fill(db);
    std::vector<flux::SearchHit> hits;
    std::string error;
    CHECK(flux::searchIndex(db, "CAT, Dog!", 10, hits, error));
    CHECK(hits.size() == 2);
}

void testRepeatedQueryTermsAreDeduplicated() {
    flux::Database db;
    fill(db);
    std::vector<flux::SearchHit> hits;
    std::string error;
    CHECK(flux::searchIndex(db, "cat cat CAT", 10, hits, error));
    CHECK(hits.size() == 3);
}

void testLimit() {
    flux::Database db;
    fill(db);
    std::vector<flux::SearchHit> hits;
    std::string error;
    CHECK(flux::searchIndex(db, "cat", 2, hits, error));
    CHECK(paths(hits) == (Paths{"/a", "/d"}));
}

void testNoMatchIsSuccessWithNoHits() {
    flux::Database db;
    fill(db);
    std::vector<flux::SearchHit> hits;
    std::string error;
    CHECK(flux::searchIndex(db, "unicorn", 10, hits, error));
    CHECK(hits.empty());
    CHECK(flux::searchIndex(db, "cat unicorn", 10, hits, error));  // one known, one unknown term
    CHECK(hits.empty());
}

void testQueryWithoutTermsIsAnError() {
    flux::Database db;
    fill(db);
    std::vector<flux::SearchHit> hits;
    std::string error;
    CHECK(!flux::searchIndex(db, "", 10, hits, error));
    CHECK(!error.empty());
    CHECK(!flux::searchIndex(db, " ,.; ", 10, hits, error));
}

void testEmptyDatabase() {
    flux::Database db;
    std::string error;
    CHECK(db.open(":memory:", flux::OpenMode::ReadWrite, error));
    std::vector<flux::SearchHit> hits;
    CHECK(flux::searchIndex(db, "anything", 10, hits, error));
    CHECK(hits.empty());
}

void testManyTermsQuery() {
    flux::Database db;
    fill(db);
    std::vector<flux::SearchHit> hits;
    std::string error;
    CHECK(flux::searchIndex(db, "a b c d e f g h i j k l m n o p cat", 10, hits, error));
    CHECK(hits.empty());  // 16 unknown terms + cat: nothing contains them all
}

#include "search_files_tests.inc"

int main() {
    RUN(testSingleTermRankedByFrequencyThenPath);
    RUN(testMultiTermIsAnd);
    RUN(testCaseInsensitiveAndPunctuation);
    RUN(testRepeatedQueryTermsAreDeduplicated);
    RUN(testLimit);
    RUN(testNoMatchIsSuccessWithNoHits);
    RUN(testQueryWithoutTermsIsAnError);
    RUN(testEmptyDatabase);
    RUN(testManyTermsQuery);
    RUN(testFilenameSearchExactPartialAndWithoutExtension);
    RUN(testFilenameSearchIsCaseInsensitiveAndIgnoresPunctuation);
    RUN(testMultipleWordsMustAllBeInTheName);
    RUN(testFolderNamesAreNotFilenameMatches);
    RUN(testNameAndContentMatchesAreCombinedAndRanked);
    RUN(testSearchFilesKeepsTheContentRulesOfSearchIndex);
    RUN(testNonAsciiFilenamesAreFound);
    RUN(testMetadataAndDisplayHelpers);
    RUN(testSameNameInDifferentFoldersAndUnknownRoot);
    RUN(testSnippetsComeFromTheFileAsItIsNow);
    return testExitCode();
}
