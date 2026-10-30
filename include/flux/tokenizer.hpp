#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace flux {

// Splits text into lowercase tokens. A token is a maximal run of ASCII letters
// and digits; every other byte (whitespace, punctuation, non-ASCII) is a
// separator. Tokens are returned in order of appearance, duplicates included.
std::vector<std::string> tokenize(std::string_view text);

// Same tokens, counted instead of listed: adds each token to `counts` without ever holding the whole token
// list in memory (large documents). Tokens longer than `maxTokenLength` (0 = no limit) are dropped: in
// extracted text they are encoded data, not words. Returns the number of tokens kept.
std::size_t tokenizeCounts(std::string_view text, std::unordered_map<std::string, std::uint32_t>& counts,
                           std::size_t maxTokenLength = 0);

}  // namespace flux
