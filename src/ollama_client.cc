#include "ollama_client.h"

#include <httplib.h>

#include <cctype>
#include <exception>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <utility>

#include "logger.h"
#include "tokenizer.h"

namespace search_engine {

namespace {
constexpr size_t kMaxNewTerms = 10;
}  // namespace

OllamaClient::OllamaClient(std::string host, std::string model,
                           std::chrono::milliseconds timeout)
    : host_(std::move(host)), model_(std::move(model)), timeout_(timeout) {}

namespace {

ExpansionResult Fallback(const std::string& reason) {
  Logger::Instance().Warn("Ollama expansion fallback: " + reason);
  ExpansionResult result;
  result.used_fallback = true;
  return result;
}

// Removes leading whitespace, then one case-insensitive "keywords:" prefix.
std::string StripKeywordsPrefix(const std::string& text) {
  constexpr char kPrefix[] = "keywords:";
  constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
  size_t pos = 0;
  while (pos < text.size() &&
         std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
  if (text.size() - pos >= kPrefixLen) {
    bool match = true;
    for (size_t i = 0; i < kPrefixLen; ++i) {
      char c = text[pos + i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != kPrefix[i]) {
        match = false;
        break;
      }
    }
    if (match) pos += kPrefixLen;
  }
  return text.substr(pos);
}

std::string JoinTerms(const std::vector<std::string>& terms) {
  std::string joined;
  for (const std::string& term : terms) {
    if (!joined.empty()) joined += ", ";
    joined += term;
  }
  return joined;
}

}  // namespace

ExpansionResult OllamaClient::Expand(
    const std::string& original_query,
    const std::vector<std::string>& original_terms) {
  try {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = Clock::now() + timeout_;

    // Queue stage: one LLM request at a time, bounded by the shared budget.
    std::unique_lock<std::timed_mutex> lock(generate_mutex_, std::defer_lock);
    if (!lock.try_lock_until(deadline)) {
      return Fallback("timed out waiting in the LLM queue");
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              Clock::now());
    if (remaining.count() <= 0) {
      return Fallback("budget exhausted before the HTTP call");
    }

    // HTTP stage: the client timeouts equal the remaining budget.
    httplib::Client client(host_);
    client.set_connection_timeout(remaining);
    client.set_read_timeout(remaining);
    client.set_write_timeout(remaining);

    const nlohmann::json body = {
        {"model", model_},
        {"prompt", BuildPrompt(original_query)},
        {"stream", false},
        {"options", {{"temperature", 0}, {"num_predict", 64}}}};
    httplib::Result http = client.Post(
        "/api/generate",
        body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
        "application/json");
    lock.unlock();

    if (!http) {
      return Fallback("Ollama unavailable or timed out (" +
                      httplib::to_string(http.error()) + ")");
    }
    if (http->status != 200) {
      return Fallback("Ollama returned HTTP " + std::to_string(http->status));
    }
    const nlohmann::json json =
        nlohmann::json::parse(http->body, nullptr, false);
    if (!json.is_object() || !json.contains("response") ||
        !json["response"].is_string()) {
      return Fallback("malformed Ollama response");
    }

    const std::string response =
        StripKeywordsPrefix(json["response"].get<std::string>());
    std::unordered_set<std::string> seen(original_terms.begin(),
                                         original_terms.end());
    ExpansionResult result;
    for (const std::string& token : Tokenizer::Tokenize(response)) {
      if (result.new_terms.size() >= kMaxNewTerms) break;
      if (seen.insert(token).second) result.new_terms.push_back(token);
    }
    if (result.new_terms.empty()) {
      return Fallback("no new terms after parsing the response");
    }
    Logger::Instance().Info("Ollama expansion new_terms: [" +
                            JoinTerms(result.new_terms) + "]");
    return result;
  } catch (const std::exception& e) {
    return Fallback(std::string("exception: ") + e.what());
  } catch (...) {
    return Fallback("unknown exception");
  }
}

// Stub: the /api/tags check is added later.
bool OllamaClient::IsAvailable() const { return false; }

std::string OllamaClient::BuildPrompt(const std::string& original_query) {
  return "List 5 to 7 English keywords and synonyms for this search query, "
         "separated by\ncommas, with no explanation. If the query is not in "
         "English, translate it and\nreturn English keywords instead.\n\n"
         "Query: " +
         original_query + "\n\nKeywords:";
}

}  // namespace search_engine
