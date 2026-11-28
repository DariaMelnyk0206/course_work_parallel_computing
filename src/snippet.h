#ifndef SEARCH_ENGINE_SNIPPET_H_
#define SEARCH_ENGINE_SNIPPET_H_

#include <string>

namespace search_engine {

// Pure function (no I/O) that builds a document snippet from the raw file
// content (Requirement 12). The result is at most 160 bytes, starts at
// offset min(4096, size / 4), is aligned to word boundaries at both ends,
// and has whitespace runs collapsed into a single space. If the file has
// fewer than offset + 160 bytes, the beginning of the file is used. An
// empty file yields an empty snippet.
std::string MakeSnippet(const std::string& content);

}  // namespace search_engine

#endif  // SEARCH_ENGINE_SNIPPET_H_
