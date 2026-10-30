#include "dataset_generator.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace flux::bench {
namespace {

constexpr char kConsonants[] = "bdfgklmnprstvz";  // 14
constexpr char kVowels[] = "aeiou";               // 5
constexpr std::size_t kSyllables = 14 * 5;        // 70; three syllables give 343000 distinct words

// SplitMix64: tiny, fully specified, identical on every platform.
std::uint64_t nextRandom(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint64_t fileSeed(std::uint64_t seed, std::size_t index) {
    std::uint64_t state = seed ^ ((static_cast<std::uint64_t>(index) + 1) * 0xD1B54A32D192ED03ULL);
    nextRandom(state);  // decorrelate neighbouring indices
    return state;
}

std::string formatIndex(const char* prefix, std::size_t value, int width) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%s%0*zu", prefix, width, value);
    return buffer;
}

}  // namespace

std::string validate(const DatasetConfig& c) {
    if (c.files == 0) return "files must be at least 1";
    if (c.wordsPerFile == 0) return "words per file must be at least 1";
    if (c.vocabularySize == 0 || c.vocabularySize > kMaxVocabulary) {
        return "vocabulary must be between 1 and " + std::to_string(kMaxVocabulary);
    }
    if (c.directories == 0) return "directories must be at least 1";
    return "";
}

std::string DatasetGenerator::wordForRank(std::size_t rank) {
    std::size_t value = rank - 1;
    std::string word(6, 'a');
    for (int syllable = 2; syllable >= 0; --syllable) {
        const std::size_t digit = value % kSyllables;
        value /= kSyllables;
        word[static_cast<std::size_t>(syllable) * 2] = kConsonants[digit / 5];
        word[static_cast<std::size_t>(syllable) * 2 + 1] = kVowels[digit % 5];
    }
    return word;
}

DatasetGenerator::DatasetGenerator(const DatasetConfig& config) : config_(config) {
    vocabulary_.reserve(config_.vocabularySize);
    cumulativeWeight_.reserve(config_.vocabularySize);
    std::uint64_t total = 0;
    for (std::size_t rank = 1; rank <= config_.vocabularySize; ++rank) {
        vocabulary_.push_back(wordForRank(rank));
        total += 1000000000ULL / rank;  // integer Zipf weight, no floating point involved
        cumulativeWeight_.push_back(total);
    }
}

std::size_t DatasetGenerator::sampleRank(std::uint64_t random) const {
    const std::uint64_t x = random % cumulativeWeight_.back();
    const auto it = std::upper_bound(cumulativeWeight_.begin(), cumulativeWeight_.end(), x);
    return static_cast<std::size_t>(it - cumulativeWeight_.begin()) + 1;
}

fs::path DatasetGenerator::relativePath(std::size_t index) const {
    const char* extension = index % 5 == 4 ? ".md" : ".txt";
    return fs::path(formatIndex("d", index % config_.directories, 3)) /
           (formatIndex("file_", index, 6) + extension);
}

std::string DatasetGenerator::content(std::size_t index, std::uint64_t& words) const {
    std::uint64_t state = fileSeed(config_.seed, index);
    const std::size_t low = std::max<std::size_t>(config_.wordsPerFile / 2, 1);
    const std::size_t count = low + static_cast<std::size_t>(nextRandom(state) % (config_.wordsPerFile + 1));

    std::string text;
    text.reserve(count * 9);
    std::size_t sentenceLeft = 8 + static_cast<std::size_t>(nextRandom(state) % 8);
    std::size_t sentencesInParagraph = 0;
    bool sentenceStart = true;

    for (std::size_t i = 0; i < count; ++i) {
        std::string word = vocabulary_[sampleRank(nextRandom(state)) - 1];
        if (sentenceStart) {
            word[0] = static_cast<char>(word[0] - 'a' + 'A');  // exercises case normalization
            sentenceStart = false;
        }
        text += word;
        --sentenceLeft;

        if (i + 1 == count) {
            text += ".\n";
        } else if (sentenceLeft == 0) {
            text += (++sentencesInParagraph % 3 == 0) ? ".\n" : ". ";
            sentenceLeft = 8 + static_cast<std::size_t>(nextRandom(state) % 8);
            sentenceStart = true;
        } else if (sentenceLeft > 2 && nextRandom(state) % 7 == 0) {
            text += ", ";
        } else {
            text += ' ';
        }
    }
    words = count;
    return text;
}

bool readManifest(const fs::path& directory, DatasetManifest& manifest, std::string& error) {
    std::ifstream in(directory / kManifestFileName);
    if (!in) {
        error = "no " + std::string(kManifestFileName) + " in " + directory.string();
        return false;
    }
    std::map<std::string, std::uint64_t> values;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        try {
            values[line.substr(0, eq)] = std::stoull(line.substr(eq + 1));
        } catch (const std::exception&) {
            error = "malformed manifest line: " + line;
            return false;
        }
    }
    for (const char* key : {"format", "files", "words_per_file", "vocabulary", "seed", "directories",
                            "total_words", "total_bytes"}) {
        if (values.count(key) == 0) {
            error = std::string("manifest is missing '") + key + "'";
            return false;
        }
    }
    if (values["format"] != 1) {
        error = "unsupported manifest format";
        return false;
    }
    manifest.config.files = values["files"];
    manifest.config.wordsPerFile = values["words_per_file"];
    manifest.config.vocabularySize = values["vocabulary"];
    manifest.config.seed = values["seed"];
    manifest.config.directories = values["directories"];
    manifest.totalWords = values["total_words"];
    manifest.totalBytes = values["total_bytes"];
    return true;
}

bool generateDataset(const fs::path& directory, const DatasetConfig& config,
                     DatasetManifest& manifest, bool& reused, std::string& error) {
    reused = false;
    const std::string problem = validate(config);
    if (!problem.empty()) {
        error = problem;
        return false;
    }

    std::error_code ec;
    if (fs::exists(directory, ec)) {
        if (!fs::is_directory(directory, ec)) {
            error = "not a directory: " + directory.string();
            return false;
        }
        if (!fs::is_empty(directory, ec)) {
            std::string readError;
            DatasetManifest existing;
            if (readManifest(directory, existing, readError) && existing.config == config) {
                manifest = existing;
                reused = true;
                return true;
            }
            error = "refusing to write into non-empty directory " + directory.string() +
                    " (Flux never overwrites or deletes existing files). Choose a new or empty "
                    "directory, or remove this one yourself.";
            return false;
        }
    }
    fs::create_directories(directory, ec);
    if (ec) {
        error = "cannot create " + directory.string() + ": " + ec.message();
        return false;
    }

    const DatasetGenerator generator(config);
    manifest = DatasetManifest{};
    manifest.config = config;
    for (std::size_t i = 0; i < config.files; ++i) {
        const fs::path file = directory / generator.relativePath(i);
        fs::create_directories(file.parent_path(), ec);
        if (ec) {
            error = "cannot create " + file.parent_path().string() + ": " + ec.message();
            return false;
        }
        if (fs::exists(file, ec)) {  // cannot happen in an empty directory; checked anyway
            error = "would overwrite existing file " + file.string();
            return false;
        }
        std::uint64_t words = 0;
        const std::string text = generator.content(i, words);
        std::ofstream out(file, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            error = "cannot write " + file.string();
            return false;
        }
        manifest.totalWords += words;
        manifest.totalBytes += text.size();
    }

    std::ofstream out(directory / kManifestFileName, std::ios::binary);
    out << "format=1\n"
        << "files=" << config.files << '\n'
        << "words_per_file=" << config.wordsPerFile << '\n'
        << "vocabulary=" << config.vocabularySize << '\n'
        << "seed=" << config.seed << '\n'
        << "directories=" << config.directories << '\n'
        << "total_words=" << manifest.totalWords << '\n'
        << "total_bytes=" << manifest.totalBytes << '\n';
    out.close();
    if (!out) {
        error = "cannot write manifest in " + directory.string();
        return false;
    }
    return true;
}

}  // namespace flux::bench
