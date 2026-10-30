#include "dataset_generator.hpp"
#include "flux/indexer.hpp"
#include "flux/tokenizer.hpp"
#include "test_util.hpp"

#include <set>

namespace fs = std::filesystem;
using namespace flux::bench;

namespace {

DatasetConfig smallConfig() {
    DatasetConfig c;
    c.files = 30;
    c.wordsPerFile = 80;
    c.vocabularySize = 200;
    c.seed = 7;
    c.directories = 4;
    return c;
}

std::string readFile(const fs::path& p) {
    std::string content;
    flux::readTextFile(p, content);
    return content;
}

}  // namespace

void testSameSeedSameContent() {
    const DatasetGenerator a(smallConfig()), b(smallConfig());
    for (std::size_t i = 0; i < 30; ++i) {
        std::uint64_t wa = 0, wb = 0;
        CHECK(a.content(i, wa) == b.content(i, wb));
        CHECK(wa == wb);
        CHECK(a.relativePath(i) == b.relativePath(i));
    }
}

void testContentDoesNotDependOnGenerationOrder() {
    const DatasetGenerator g(smallConfig());
    std::uint64_t w1 = 0, w2 = 0, w3 = 0;
    const std::string first = g.content(5, w1);
    g.content(2, w2);
    g.content(9, w2);
    CHECK(g.content(5, w3) == first && w3 == w1);
}

void testDifferentSeedDifferentContent() {
    DatasetConfig other = smallConfig();
    other.seed = 8;
    const DatasetGenerator a(smallConfig()), b(other);
    std::uint64_t wa = 0, wb = 0;
    CHECK(a.content(0, wa) != b.content(0, wb));
}

void testFilesDifferFromEachOther() {
    const DatasetGenerator g(smallConfig());
    std::set<std::string> seen;
    std::uint64_t w = 0;
    for (std::size_t i = 0; i < 30; ++i) seen.insert(g.content(i, w));
    CHECK(seen.size() == 30);
}

void testWordCountMatchesTokenizerAndStaysInRange() {
    const DatasetConfig c = smallConfig();
    const DatasetGenerator g(c);
    for (std::size_t i = 0; i < c.files; ++i) {
        std::uint64_t words = 0;
        const std::string text = g.content(i, words);
        CHECK(flux::tokenize(text).size() == words);  // expected token count is exact
        CHECK(words >= c.wordsPerFile / 2 && words <= c.wordsPerFile / 2 + c.wordsPerFile);
    }
}

void testWordsAreDistinctPerRank() {
    std::set<std::string> words;
    for (std::size_t r = 1; r <= kMaxVocabulary; ++r) words.insert(DatasetGenerator::wordForRank(r));
    CHECK(words.size() == kMaxVocabulary);
    CHECK(DatasetGenerator::wordForRank(1) == DatasetGenerator::wordForRank(1));
}

void testZipfSkewMakesLowRanksCommon() {
    const DatasetConfig c = smallConfig();
    const DatasetGenerator g(c);
    std::size_t common = 0, rare = 0;
    const std::string commonWord = DatasetGenerator::wordForRank(1);
    const std::string rareWord = DatasetGenerator::wordForRank(200);
    for (std::size_t i = 0; i < c.files; ++i) {
        std::uint64_t w = 0;
        for (const auto& t : flux::tokenize(g.content(i, w))) {
            common += t == commonWord;
            rare += t == rareWord;
        }
    }
    CHECK(common > 10 * (rare + 1));
}

void testValidation() {
    DatasetConfig c = smallConfig();
    CHECK(validate(c).empty());
    c.files = 0;
    CHECK(!validate(c).empty());
    c = smallConfig();
    c.vocabularySize = kMaxVocabulary + 1;
    CHECK(!validate(c).empty());
    c = smallConfig();
    c.directories = 0;
    CHECK(!validate(c).empty());
}

void testGenerateWritesManifestAndMatchingFiles() {
    TempDir dir("gen_basic");
    const fs::path out = dir.path() / "data";
    DatasetManifest m;
    bool reused = true;
    std::string error;
    CHECK(generateDataset(out, smallConfig(), m, reused, error));
    CHECK(!reused);
    CHECK(fs::exists(out / kManifestFileName));

    DatasetManifest read;
    CHECK(readManifest(out, read, error));
    CHECK(read.config == smallConfig());
    CHECK(read.totalWords == m.totalWords && read.totalBytes == m.totalBytes);

    // The files on disk are exactly what the generator says, and Flux counts the manifest's tokens.
    flux::IndexStats stats;
    CHECK(flux::indexDirectory(out, stats, error, 4));
    CHECK(stats.documents == smallConfig().files);
    CHECK(stats.tokens == m.totalWords);
    CHECK(stats.skipped.empty());
    CHECK(stats.unsupportedFiles == 1);  // exactly the manifest file, which is never indexed
}

void testGenerationIsByteForByteRepeatable() {
    TempDir dir("gen_repeat");
    DatasetManifest m1, m2;
    bool reused = false;
    std::string error;
    CHECK(generateDataset(dir.path() / "a", smallConfig(), m1, reused, error));
    CHECK(generateDataset(dir.path() / "b", smallConfig(), m2, reused, error));
    CHECK(m1.totalWords == m2.totalWords && m1.totalBytes == m2.totalBytes);
    const DatasetGenerator g(smallConfig());
    for (std::size_t i = 0; i < 30; i += 7) {
        CHECK(readFile(dir.path() / "a" / g.relativePath(i)) == readFile(dir.path() / "b" / g.relativePath(i)));
    }
}

void testRefusesNonEmptyDirectoryAndKeepsUserFiles() {
    TempDir dir("gen_safety");
    dir.write("data/precious.txt", "do not touch");
    DatasetManifest m;
    bool reused = false;
    std::string error;
    CHECK(!generateDataset(dir.path() / "data", smallConfig(), m, reused, error));
    CHECK(!error.empty());
    CHECK(readFile(dir.path() / "data" / "precious.txt") == "do not touch");
    CHECK(!fs::exists(dir.path() / "data" / kManifestFileName));
    std::size_t entries = 0;
    for (const auto& e : fs::directory_iterator(dir.path() / "data")) { (void)e; ++entries; }
    CHECK(entries == 1);  // nothing was added either
}

void testExistingDatasetIsReusedOnlyWithSameConfig() {
    TempDir dir("gen_reuse");
    const fs::path out = dir.path() / "data";
    DatasetManifest m;
    bool reused = false;
    std::string error;
    CHECK(generateDataset(out, smallConfig(), m, reused, error));
    CHECK(generateDataset(out, smallConfig(), m, reused, error));
    CHECK(reused);

    DatasetConfig different = smallConfig();
    different.files = 31;
    CHECK(!generateDataset(out, different, m, reused, error));  // refused, not overwritten
    CHECK(fs::exists(out / "d000"));
}

void testAcceptsEmptyExistingDirectoryAndRejectsAFile() {
    TempDir dir("gen_empty_dir");
    fs::create_directories(dir.path() / "empty");
    DatasetManifest m;
    bool reused = false;
    std::string error;
    CHECK(generateDataset(dir.path() / "empty", smallConfig(), m, reused, error));
    dir.write("afile", "x");
    CHECK(!generateDataset(dir.path() / "afile", smallConfig(), m, reused, error));
}

int main() {
    RUN(testSameSeedSameContent);
    RUN(testContentDoesNotDependOnGenerationOrder);
    RUN(testDifferentSeedDifferentContent);
    RUN(testFilesDifferFromEachOther);
    RUN(testWordCountMatchesTokenizerAndStaysInRange);
    RUN(testWordsAreDistinctPerRank);
    RUN(testZipfSkewMakesLowRanksCommon);
    RUN(testValidation);
    RUN(testGenerateWritesManifestAndMatchingFiles);
    RUN(testGenerationIsByteForByteRepeatable);
    RUN(testRefusesNonEmptyDirectoryAndKeepsUserFiles);
    RUN(testExistingDatasetIsReusedOnlyWithSameConfig);
    RUN(testAcceptsEmptyExistingDirectoryAndRejectsAFile);
    return testExitCode();
}
