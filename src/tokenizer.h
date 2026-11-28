#ifndef SEARCH_ENGINE_TOKENIZER_H_
#define SEARCH_ENGINE_TOKENIZER_H_

#include <string>
#include <vector>

namespace search_engine {

// Single normalization function shared by indexing, query processing, and
// LLM response parsing (Requirement 6.6). A token is a maximal run of ASCII
// [A-Za-z0-9], lowercased; every other byte -- including UTF-8 bytes >= 0x80
// -- is a separator. Tokens shorter than 2 or longer than 64 characters are
// dropped. Stop words are kept (Requirement 6.1-6.5).
class Tokenizer {
 public:
  // Returns tokens in order of first appearance; duplicates are preserved
  // (callers deduplicate when needed, e.g. for original_terms).
  static std::vector<std::string> Tokenize(const std::string& text);
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_TOKENIZER_H_
