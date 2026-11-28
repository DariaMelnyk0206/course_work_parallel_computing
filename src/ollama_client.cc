#include "ollama_client.h"

#include <utility>

namespace search_engine {

OllamaClient::OllamaClient(std::string host, std::string model,
                           std::chrono::milliseconds timeout)
    : host_(std::move(host)), model_(std::move(model)), timeout_(timeout) {}

// Stub: always returns the fallback result; the HTTP call is added later.
ExpansionResult OllamaClient::Expand(
    const std::string& original_query,
    const std::vector<std::string>& original_terms) {
  (void)original_query;
  (void)original_terms;
  ExpansionResult result;
  result.used_fallback = true;
  return result;
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
