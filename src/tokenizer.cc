#include "tokenizer.h"

#include <cstddef>
#include <utility>

namespace search_engine {

namespace {

constexpr size_t kMinTokenLength = 2;
constexpr size_t kMaxTokenLength = 64;

bool IsAsciiAlnum(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

char ToLowerAscii(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
  return static_cast<char>(c);
}

}  // namespace

std::vector<std::string> Tokenizer::Tokenize(const std::string& text) {
  std::vector<std::string> tokens;
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    if (!IsAsciiAlnum(static_cast<unsigned char>(text[i]))) {
      ++i;
      continue;
    }
    size_t start = i;
    while (i < n && IsAsciiAlnum(static_cast<unsigned char>(text[i]))) ++i;
    size_t len = i - start;
    // The whole maximal run is dropped when out of range (not split).
    if (len < kMinTokenLength || len > kMaxTokenLength) continue;
    std::string token;
    token.reserve(len);
    for (size_t j = start; j < i; ++j) {
      token.push_back(ToLowerAscii(static_cast<unsigned char>(text[j])));
    }
    tokens.push_back(std::move(token));
  }
  return tokens;
}

std::vector<TokenOffset> Tokenizer::TokenizeWithOffsets(
    const std::string& text) {
  std::vector<TokenOffset> tokens;
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    if (!IsAsciiAlnum(static_cast<unsigned char>(text[i]))) {
      ++i;
      continue;
    }
    size_t start = i;
    while (i < n && IsAsciiAlnum(static_cast<unsigned char>(text[i]))) ++i;
    size_t len = i - start;
    if (len < kMinTokenLength || len > kMaxTokenLength) continue;
    std::string token;
    token.reserve(len);
    for (size_t j = start; j < i; ++j) {
      token.push_back(ToLowerAscii(static_cast<unsigned char>(text[j])));
    }
    tokens.push_back({std::move(token), static_cast<uint32_t>(start)});
  }
  return tokens;
}

}  // namespace search_engine
