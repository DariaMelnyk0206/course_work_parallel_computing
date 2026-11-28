#ifndef SEARCH_ENGINE_TOKENIZER_H_
#define SEARCH_ENGINE_TOKENIZER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace search_engine {

// Single normalization function shared by indexing, query processing, and
// LLM response parsing (Requirement 6.6). A token is a maximal run of ASCII
// [A-Za-z0-9], lowercased; every other byte -- including UTF-8 bytes >= 0x80
// -- is a separator. Tokens shorter than 2 or longer than 64 characters are
// dropped. Stop words are kept (Requirement 6.1-6.5).
// A token together with the byte offset of its first character in the input.
struct TokenOffset {
  std::string token;
  uint32_t offset;
};

class Tokenizer {
 public:
  // Returns tokens in order of first appearance; duplicates are preserved
  // (callers deduplicate when needed, e.g. for original_terms).
  static std::vector<std::string> Tokenize(const std::string& text);

  // Same rules and token sequence as Tokenize, but each token carries the
  // byte offset of its start in `text` (Requirement 6.8).
  static std::vector<TokenOffset> TokenizeWithOffsets(const std::string& text);
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_TOKENIZER_H_
