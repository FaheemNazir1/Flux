#include "flux/tokenizer.hpp"

namespace flux {
namespace {

bool isAsciiAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

char toAsciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char c : text) {
        if (isAsciiAlnum(c)) {
            current.push_back(toAsciiLower(c));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

std::size_t tokenizeCounts(std::string_view text, std::unordered_map<std::string, std::uint32_t>& counts,
                           std::size_t maxTokenLength) {
    std::size_t kept = 0;
    std::string current;
    bool tooLong = false;
    const auto flush = [&] {
        if (!current.empty() && !tooLong) {
            ++counts[current];
            ++kept;
        }
        current.clear();
        tooLong = false;
    };
    for (const char c : text) {
        if (isAsciiAlnum(c)) {
            if (maxTokenLength != 0 && current.size() >= maxTokenLength) {
                tooLong = true;
            } else if (!tooLong) {
                current.push_back(toAsciiLower(c));
            }
        } else {
            flush();
        }
    }
    flush();
    return kept;
}

}  // namespace flux
