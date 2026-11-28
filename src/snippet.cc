#include "snippet.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <ios>

namespace search_engine {

namespace {

constexpr size_t kWindow = 160;
constexpr size_t kMaxOffset = 4096;

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

constexpr size_t kHalf = 100;
constexpr size_t kMax = 160;
constexpr size_t kEllipsisLen = 3;

bool IsAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

char ToLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::optional<std::string> BuildKwicSnippet(const std::string& file_path,
                                            uint32_t offset,
                                            const std::string& term) {
  const size_t len = term.size();
  if (len == 0) return std::nullopt;

  std::ifstream file(file_path, std::ios::binary);
  if (!file) return std::nullopt;
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0) return std::nullopt;
  const size_t n = static_cast<size_t>(size);
  if (static_cast<size_t>(offset) + len > n) return std::nullopt;

  const size_t start = offset >= kHalf ? offset - kHalf : 0;
  const size_t end = std::min(n, static_cast<size_t>(offset) + len + kHalf);
  std::string window(end - start, '\0');
  file.seekg(static_cast<std::streamoff>(start), std::ios::beg);
  file.read(window.data(), static_cast<std::streamsize>(window.size()));
  if (!file || static_cast<size_t>(file.gcount()) != window.size()) {
    return std::nullopt;
  }
  const bool left_open = start > 0;
  const bool right_open = end < n;

  // Verify the anchor term stands at the offset on token boundaries.
  const size_t a = offset - start;
  for (size_t i = 0; i < len; ++i) {
    if (ToLower(window[a + i]) != term[i]) return std::nullopt;
  }
  if (a > 0 && IsAlnum(window[a - 1])) return std::nullopt;
  if (a + len < window.size() && IsAlnum(window[a + len])) return std::nullopt;

  // Collapse whitespace runs, trim, and track the anchor span [ca, ce).
  std::string c;
  c.reserve(window.size());
  bool pending_space = false;
  size_t ca = 0;
  size_t ce = 0;
  for (size_t i = 0; i < window.size(); ++i) {
    if (IsSpace(window[i])) {
      pending_space = !c.empty();
      continue;
    }
    if (pending_space) c.push_back(' ');
    pending_space = false;
    if (i == a) ca = c.size();
    c.push_back(window[i]);
    if (i == a + len - 1) ce = c.size();
  }

  size_t l = 0;
  size_t r = c.size();
  bool lc = left_open;
  bool rc = right_open;
  if ((r - l) + kEllipsisLen * (lc ? 1 : 0) + kEllipsisLen * (rc ? 1 : 0) >
      kMax) {
    const size_t side = (kMax - 2 * kEllipsisLen - (ce - ca)) / 2;
    size_t extra_left = 0;
    size_t extra_right = 0;
    if (ca < side) extra_right = side - ca;
    if (c.size() - ce < side) extra_left = side - (c.size() - ce);
    l = ca > side + extra_left ? ca - side - extra_left : 0;
    r = std::min(c.size(), ce + side + extra_right);
    lc = left_open || l > 0;
    rc = right_open || r < c.size();
  }

  // Align to word boundaries on the sides that are cut.
  if (lc && (l == 0 || (c[l - 1] != ' ' && c[l] != ' '))) {
    size_t pos = l;
    while (pos < ca && c[pos] != ' ') ++pos;
    l = pos < ca ? pos + 1 : ca;
  }
  if (rc && (r == c.size() || (c[r - 1] != ' ' && c[r] != ' '))) {
    size_t pos = r;
    while (pos > ce && c[pos - 1] != ' ') --pos;
    // pos is the start of the trailing partial word (or ce).
    r = pos > ce ? pos - 1 : ce;
  }
  while (l < ca && c[l] == ' ') ++l;
  while (r > ce && c[r - 1] == ' ') --r;

  std::string result;
  if (lc) result += "...";
  result.append(c, l, r - l);
  if (rc) result += "...";
  return result;
}

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
