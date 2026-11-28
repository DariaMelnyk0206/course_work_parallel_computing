#include "index_builder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "document_table.h"
#include "logger.h"
#include "snippet.h"
#include "striped_hash_table.h"
#include "thread_pool.h"
#include "tokenizer.h"

namespace search_engine {

namespace {

namespace fs = std::filesystem;

// Sequential scan: recursive "*.txt" lookup, sorted by relative path so that
// doc_id assignment is deterministic (Requirement 2.9).
std::vector<fs::path> ScanDataset(const std::string& dataset_dir) {
  std::vector<std::pair<std::string, fs::path>> found;
  std::error_code ec;
  const fs::path root(dataset_dir);
  if (!fs::is_directory(root, ec)) {
    return {};
  }
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (!ec && it != end) {
    std::error_code entry_ec;
    if (it->is_regular_file(entry_ec) && !entry_ec &&
        it->path().extension() == ".txt") {
      found.emplace_back(it->path().lexically_relative(root).generic_string(),
                         it->path());
    }
    it.increment(ec);
  }
  if (ec) {
    Logger::Instance().Warn("Dataset scan stopped early: " + ec.message());
  }
  std::sort(found.begin(), found.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<fs::path> files;
  files.reserve(found.size());
  for (auto& entry : found) {
    files.push_back(std::move(entry.second));
  }
  return files;
}

// Reads the whole file in binary mode. Returns false on any I/O failure.
bool ReadFile(const fs::path& path, std::string* content) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  content->assign(std::istreambuf_iterator<char>(in),
                  std::istreambuf_iterator<char>());
  return !in.bad();
}

// One task per file. Touches shared state only through InsertBatch (stripe
// lock), its own DocumentTable slot, and the atomic counter.
void IndexOneFile(const fs::path& path, uint32_t doc_id,
                  StripedHashTable* table, DocumentTable* documents,
                  std::atomic<uint32_t>* indexed) {
  std::string content;
  if (!ReadFile(path, &content)) {
    Logger::Instance().Warn("Skipping unreadable file: " + path.string());
    return;  // Slot stays default-constructed with valid == false.
  }

  DocumentInfo info;
  info.name = path.filename().string();
  info.snippet = MakeSnippet(content);
  info.valid = true;

  // Local aggregation, no locks (Requirement 9.4).
  std::vector<std::string> tokens = Tokenizer::Tokenize(content);
  std::unordered_set<std::string> unique_terms(
      std::make_move_iterator(tokens.begin()),
      std::make_move_iterator(tokens.end()));
  tokens.clear();
  tokens.shrink_to_fit();

  // Group by stripe so each stripe is locked once per document.
  std::unordered_map<size_t, std::vector<StripedInsertItem>> by_stripe;
  for (const std::string& term : unique_terms) {
    const uint64_t hash = StripedHashTable::Fnv1aHash64(term);
    by_stripe[table->StripeFor(hash)].push_back({term, hash, doc_id});
  }
  for (const auto& [stripe, items] : by_stripe) {
    table->InsertBatch(stripe, items);
  }

  documents->Set(doc_id, std::move(info));
  indexed->fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

IndexBuildResult IndexBuilder::Build(const std::string& dataset_dir,
                                     int num_threads) {
  const auto start = std::chrono::steady_clock::now();

  // Sequential part (Amdahl): scan and sort.
  const std::vector<fs::path> files = ScanDataset(dataset_dir);
  if (files.empty()) {
    Logger::Instance().Warn("No .txt files found in dataset directory: " +
                            dataset_dir);
  }

  StripedHashTable table;
  DocumentTable documents(files.size());
  std::atomic<uint32_t> indexed{0};

  {
    ThreadPool index_pool(static_cast<size_t>(std::max(1, num_threads)), 0);
    std::vector<std::future<void>> futures;
    futures.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
      futures.push_back(
          index_pool.Submit([&files, &table, &documents, &indexed, i]() {
            IndexOneFile(files[i], static_cast<uint32_t>(i), &table, &documents,
                         &indexed);
          }));
    }
    // Barrier and happens-before for the DocumentTable slots.
    std::exception_ptr first_error;
    for (auto& f : futures) {
      try {
        f.get();
      } catch (...) {
        if (!first_error) first_error = std::current_exception();
      }
    }
    if (first_error) {
      std::rethrow_exception(first_error);
    }
  }

  const uint32_t count = indexed.load(std::memory_order_relaxed);
  Logger::Instance().Info(
      "Index built: " + std::to_string(count) + " documents, " +
      std::to_string(table.UniqueTermCount()) + " unique terms");

  IndexBuildResult result;
  result.documents_indexed = count;
  result.index = std::make_shared<const InvertedIndex>(
      std::move(table), std::move(documents), count);
  result.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
  return result;
}

}  // namespace search_engine
