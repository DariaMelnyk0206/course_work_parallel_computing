#include "snippet.h"

#include <algorithm>
#include <cstddef>

namespace search_engine {

namespace {

constexpr size_t kWindow = 160;
constexpr size_t kMaxOffset = 4096;

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

}  // namespace

std::string MakeSnippet(const std::string& content) {
  const size_t n = content.size();
  const size_t offset = std::min(kMaxOffset, n / 4);
  size_t start = (n < offset + kWindow) ? 0 : offset;

  // 1. Align the start to a word boundary.
  if (start > 0 && start < n && !IsSpace(content[start - 1]) &&
      !IsSpace(content[start])) {
    while (start < n && !IsSpace(content[start])) ++start;
  }
  while (start < n && IsSpace(content[start])) ++start;
  size_t end = std::min(n, start + kWindow);

  // 2. Align the end to a word boundary.
  if (end < n && end > start && !IsSpace(content[end]) &&
      !IsSpace(content[end - 1])) {
    size_t pos = end;
    while (pos > start && !IsSpace(content[pos - 1])) --pos;
    // pos is the start of the trailing partial word; whitespace precedes it.
    if (pos > start) end = pos;
  }

  // 3. Collapse whitespace runs and trim.
  std::string result;
  result.reserve(end > start ? end - start : 0);
  bool pending_space = false;
  for (size_t i = start; i < end; ++i) {
    if (IsSpace(content[i])) {
      pending_space = !result.empty();
    } else {
      if (pending_space) result.push_back(' ');
      pending_space = false;
      result.push_back(content[i]);
    }
  }
  return result;
}

}  // namespace search_engine
