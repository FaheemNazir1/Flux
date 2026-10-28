#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

// Minimal test helpers: no external framework required.
inline std::atomic<int>& failureCount() {
    static std::atomic<int> failures{0};
    return failures;
}

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failureCount();                                                          \
        }                                                                              \
    } while (0)

#define RUN(test)                                                                      \
    do {                                                                               \
        const int before = failureCount();                                             \
        test();                                                                        \
        std::cout << (failureCount() == before ? "[ OK ] " : "[FAIL] ") << #test << "\n"; \
    } while (0)

inline int testExitCode() { return failureCount() == 0 ? 0 : 1; }

// Creates a fresh temp directory and removes it on destruction.
class TempDir {
public:
    explicit TempDir(const std::string& name) {
        path_ = std::filesystem::temp_directory_path() / ("flux_test_" + name);
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    void write(const std::string& relative, const std::string& content) const {
        const auto file = path_ / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << content;
    }

private:
    std::filesystem::path path_;
};
