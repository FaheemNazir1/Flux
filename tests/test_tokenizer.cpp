#include "flux/tokenizer.hpp"
#include "test_util.hpp"

using Tokens = std::vector<std::string>;

void testLowercaseNormalization() {
    CHECK(flux::tokenize("HELLO") == Tokens{"hello"});
}

void testMixedCase() {
    CHECK(flux::tokenize("FluX sEaRcH") == (Tokens{"flux", "search"}));
}

void testPunctuationBoundaries() {
    CHECK(flux::tokenize("Hello, world! (foo-bar) a.b;c") ==
          (Tokens{"hello", "world", "foo", "bar", "a", "b", "c"}));
}

void testDigitsAreTokenCharacters() {
    CHECK(flux::tokenize("utf8 v2.0") == (Tokens{"utf8", "v2", "0"}));
}

void testWhitespaceVariants() {
    CHECK(flux::tokenize("a\tb\nc\r\nd   e") == (Tokens{"a", "b", "c", "d", "e"}));
}

void testEmptyInput() {
    CHECK(flux::tokenize("").empty());
    CHECK(flux::tokenize("   \n\t ").empty());
    CHECK(flux::tokenize("!!! ... ---").empty());
}

void testRepeatedWordsKeptInOrder() {
    CHECK(flux::tokenize("the The THE cat the") == (Tokens{"the", "the", "the", "cat", "the"}));
}

void testNonAsciiBytesAreSeparators() {
    CHECK(flux::tokenize("caf\xC3\xA9 bar") == (Tokens{"caf", "bar"}));
}

void testLeadingAndTrailingSeparators() {
    CHECK(flux::tokenize("--edge--") == Tokens{"edge"});
}

int main() {
    RUN(testLowercaseNormalization);
    RUN(testMixedCase);
    RUN(testPunctuationBoundaries);
    RUN(testDigitsAreTokenCharacters);
    RUN(testWhitespaceVariants);
    RUN(testEmptyInput);
    RUN(testRepeatedWordsKeptInOrder);
    RUN(testNonAsciiBytesAreSeparators);
    RUN(testLeadingAndTrailingSeparators);
    return testExitCode();
}
