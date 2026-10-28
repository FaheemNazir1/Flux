#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace flux {

// Splits text into lowercase tokens. A token is a maximal run of ASCII letters
// and digits; every other byte (whitespace, punctuation, non-ASCII) is a
// separator. Tokens are returned in order of appearance, duplicates included.
std::vector<std::string> tokenize(std::string_view text);

}  // namespace flux
