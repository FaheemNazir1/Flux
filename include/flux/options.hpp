#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace flux {

constexpr std::size_t kMaxThreads = 256;

struct CliOptions {
    std::filesystem::path directory;
    std::size_t threads = 0;  // filled with defaultThreadCount() by parseArguments
    bool help = false;
};

// Hardware concurrency, or 1 if it cannot be determined; never above kMaxThreads.
std::size_t defaultThreadCount();

// Accepts a plain decimal integer in [1, kMaxThreads]. No signs, spaces or suffixes.
bool parseThreadCount(std::string_view text, std::size_t& threads, std::string& error);

// Parses everything after the program name:
//   [--threads N | --threads=N] <directory>      or      -h | --help
bool parseArguments(const std::vector<std::string>& args, CliOptions& options, std::string& error);

}  // namespace flux
