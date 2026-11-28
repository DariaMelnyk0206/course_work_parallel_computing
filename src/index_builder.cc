#include "index_builder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
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
struct ScannedFile {
  std::string relative_path;
  fs::path path;
};

std::vector<ScannedFile> ScanDataset(const std::string& dataset_dir) {
  std::vector<ScannedFile> found;
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
      found.push_back(
          {it->path().lexically_relative(root).generic_string(), it->path()});
    }
    it.increment(ec);
  }
  if (ec) {
    Logger::Instance().Warn("Dataset scan stopped early: " + ec.message());
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
    return a.relative_path < b.relative_path;
  });
  return found;
}

bool AllDigits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return c >= '0' && c <= '9';
  });
}

// Loads DATASET_DIR/metadata.csv into id -> "<title> (<year>)". A missing
// file is not an error (Requirement 9.13).
std::unordered_map<std::string, std::string> LoadTitles(
    const std::string& dataset_dir) {
  std::unordered_map<std::string, std::string> titles;
  const fs::path path = fs::path(dataset_dir) / "metadata.csv";
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    Logger::Instance().Info("metadata.csv not found in " + dataset_dir +
                            "; book titles will be empty");
    return titles;
  }
  std::string line;
  size_t line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::optional<IndexBuilder::MetadataRow> row =
        IndexBuilder::ParseMetadataLine(line);
    if (!row) {
      Logger::Instance().Warn("Skipping malformed metadata.csv line " +
                              std::to_string(line_no));
      continue;
    }
    titles[row->id] = row->title + " (" + row->year + ")";
  }
  Logger::Instance().Info("Loaded " + std::to_string(titles.size()) +
                          " book titles from metadata.csv");
  return titles;
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
void IndexOneFile(const ScannedFile& file, const std::string& title,
                  uint32_t doc_id, StripedHashTable* table,
                  DocumentTable* documents, std::atomic<uint32_t>* indexed) {
  const fs::path& path = file.path;
  std::string content;
  if (!ReadFile(path, &content)) {
    Logger::Instance().Warn("Skipping unreadable file: " + path.string());
    return;  // Slot stays default-constructed with valid == false.
  }

  DocumentInfo info;
  info.name = path.filename().string();
  info.relative_path = file.relative_path;
  info.title = title;
  info.snippet = MakeSnippet(content);
  info.valid = true;

  // Local aggregation, no locks (Requirement 9.4).
  // term -> byte offset of its first occurrence; try_emplace keeps the first.
  std::unordered_map<std::string, uint32_t> first_offsets;
  for (TokenOffset& token : Tokenizer::TokenizeWithOffsets(content)) {
    first_offsets.try_emplace(std::move(token.token), token.offset);
  }
  // Group by stripe so each stripe is locked once per document.
  std::unordered_map<size_t, std::vector<StripedInsertItem>> by_stripe;
  for (const auto& [term, offset] : first_offsets) {
    const uint64_t hash = StripedHashTable::Fnv1aHash64(term);
    by_stripe[table->StripeFor(hash)].push_back({term, hash, doc_id, offset});
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
  const std::vector<ScannedFile> files = ScanDataset(dataset_dir);
  if (files.empty()) {
    Logger::Instance().Warn("No .txt files found in dataset directory: " +
                            dataset_dir);
  }

  // Titles are resolved here, sequentially, so tasks only read plain strings.
  const std::unordered_map<std::string, std::string> titles =
      LoadTitles(dataset_dir);
  std::vector<std::string> doc_titles(files.size());
  for (size_t i = 0; i < files.size(); ++i) {
    const std::string stem = files[i].path.stem().string();
    if (files[i].path.extension() == ".txt" && AllDigits(stem)) {
      const auto it = titles.find(stem);
      if (it != titles.end()) doc_titles[i] = it->second;
    }
  }

  StripedHashTable table;
  DocumentTable documents(files.size());
  std::atomic<uint32_t> indexed{0};

  {
    ThreadPool index_pool(static_cast<size_t>(std::max(1, num_threads)), 0);
    std::vector<std::future<void>> futures;
    futures.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
      futures.push_back(index_pool.Submit(
          [&files, &doc_titles, &table, &documents, &indexed, i]() {
            IndexOneFile(files[i], doc_titles[i], static_cast<uint32_t>(i),
                         &table, &documents, &indexed);
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

std::optional<IndexBuilder::MetadataRow> IndexBuilder::ParseMetadataLine(
    const std::string& line) {
  const size_t first = line.find(',');
  if (first == std::string::npos) return std::nullopt;
  const size_t last = line.rfind(',');
  if (last == first || last == 0) return std::nullopt;
  const size_t second_last = line.rfind(',', last - 1);
  if (second_last == std::string::npos || second_last <= first) {
    return std::nullopt;
  }
  MetadataRow row;
  row.id = line.substr(0, first);
  row.title = line.substr(first + 1, second_last - first - 1);
  row.year = line.substr(second_last + 1, last - second_last - 1);
  return row;
}

}  // namespace search_engine
