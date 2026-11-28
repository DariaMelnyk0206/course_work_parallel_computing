#ifndef SEARCH_ENGINE_SNIPPET_H_
#define SEARCH_ENGINE_SNIPPET_H_

#include <cstdint>
#include <optional>
#include <string>

namespace search_engine {

// Builds a keyword-in-context snippet from the file at `file_path` around
// `offset`, where `term` (normalized, lowercase) must stand. Returns
// std::nullopt if the file cannot be opened or read, is shorter than
// offset + term.size(), or the term is not at `offset` (Requirement
// 12.8-12.9). The result is at most 160 bytes including the ASCII "..."
// markers placed on truncated edges (Requirement 12.10). Opens its own
// std::ifstream and takes no locks (Requirement 12.7).
std::optional<std::string> BuildKwicSnippet(const std::string& file_path,
                                            uint32_t offset,
                                            const std::string& term);

// Pure function (no I/O) that builds a document snippet from the raw file
// content (Requirement 12). The result is at most 160 bytes, starts at
// offset min(4096, size / 4), is aligned to word boundaries at both ends,
// and has whitespace runs collapsed into a single space. If the file has
// fewer than offset + 160 bytes, the beginning of the file is used. An
// empty file yields an empty snippet.
std::string MakeSnippet(const std::string& content);

}  // namespace search_engine

#endif  // SEARCH_ENGINE_SNIPPET_H_
