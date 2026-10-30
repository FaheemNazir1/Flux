#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Deterministic synthetic corpus for benchmarking. Words are generated, not
// downloaded; their frequencies follow a Zipf-like distribution so that some
// terms are in almost every document and others are rare. The output depends
// only on DatasetConfig, never on the platform or the generation order.
namespace flux::bench {

struct DatasetConfig {
    std::size_t files = 1000;
    std::size_t wordsPerFile = 300;      // mean; each file has 0.5x to 1.5x this many
    std::size_t vocabularySize = 5000;   // distinct words (at most kMaxVocabulary)
    std::uint64_t seed = 42;
    std::size_t directories = 20;        // files are spread round-robin over this many folders

    bool operator==(const DatasetConfig& o) const {
        return files == o.files && wordsPerFile == o.wordsPerFile &&
               vocabularySize == o.vocabularySize && seed == o.seed && directories == o.directories;
    }
};

constexpr std::size_t kMaxVocabulary = 100000;

// What a generated dataset contains. totalWords equals the token count Flux must report.
struct DatasetManifest {
    DatasetConfig config;
    std::uint64_t totalWords = 0;
    std::uint64_t totalBytes = 0;
};

// Name of the marker file written (last) into every generated dataset directory.
constexpr const char* kManifestFileName = ".flux_dataset";

// Returns an empty string if the config is usable, otherwise the reason it is not.
std::string validate(const DatasetConfig& config);

class DatasetGenerator {
public:
    explicit DatasetGenerator(const DatasetConfig& config);  // config must be valid

    // Word for a 1-based popularity rank (rank 1 is the most frequent). Pure and stable.
    static std::string wordForRank(std::size_t rank);

    // Path of file `index` relative to the dataset root.
    std::filesystem::path relativePath(std::size_t index) const;

    // Text of file `index`. Sets `words` to the number of words (= tokens) in it.
    std::string content(std::size_t index, std::uint64_t& words) const;

    const DatasetConfig& config() const { return config_; }

private:
    std::size_t sampleRank(std::uint64_t random) const;

    DatasetConfig config_;
    std::vector<std::string> vocabulary_;      // vocabulary_[r - 1] is the word of rank r
    std::vector<std::uint64_t> cumulativeWeight_;
};

// Writes the dataset into `directory`, which must not exist or must be empty.
// Never overwrites or deletes anything: any other non-empty directory is refused.
// If the directory already holds a dataset with the same config (valid manifest),
// nothing is written and `reused` is set. The manifest is written last, so an
// interrupted run leaves a directory that is refused next time rather than trusted.
bool generateDataset(const std::filesystem::path& directory, const DatasetConfig& config,
                     DatasetManifest& manifest, bool& reused, std::string& error);

// Reads the manifest of a generated dataset.
bool readManifest(const std::filesystem::path& directory, DatasetManifest& manifest,
                  std::string& error);

}  // namespace flux::bench
