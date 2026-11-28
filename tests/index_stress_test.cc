// Stress-test driver for ThreadSanitizer runs (Requirement 28). This file
// holds the shared harness: seeded generators, a synthetic corpus in a
// temporary directory, and a failure counter that determines the exit code.
// The individual property checks are added by later tasks.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Fixed seeds: every run is reproducible, and each seed is logged before use.
constexpr uint32_t kSeeds[] = {1, 7, 42, 1234, 2026};

std::atomic<int> g_failures{0};

void Log(const std::string& message) { std::cerr << message << std::endl; }

// Records a failed check. Thread-safe, so worker threads may call it.
void Check(bool condition, const std::string& description) {
  if (!condition) {
    ++g_failures;
    Log("FAIL: " + description);
  }
}

int FailureCount() { return g_failures.load(); }

// Creates a generator for `seed` and logs the seed so a failure can be
// reproduced.
std::mt19937 MakeRng(uint32_t seed) {
  Log("seed=" + std::to_string(seed));
  return std::mt19937(seed);
}

// Random integer in [lo, hi].
int RandomInt(std::mt19937& rng, int lo, int hi) {
  return std::uniform_int_distribution<int>(lo, hi)(rng);
}

// Temporary directory removed on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    std::random_device rd;
    path_ = fs::temp_directory_path() /
            ("index_stress_" + tag + "_" + std::to_string(rd()));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

// Generates a vocabulary of `size` distinct lowercase tokens ("w0000", ...).
std::vector<std::string> MakeVocabulary(int size) {
  std::vector<std::string> vocab;
  vocab.reserve(size);
  for (int i = 0; i < size; ++i) {
    std::string n = std::to_string(i);
    vocab.push_back("w" + std::string(4 - std::min<size_t>(4, n.size()), '0') +
                    n);
  }
  return vocab;
}

// Writes `num_docs` .txt files into `dir`. Each has 1..`max_words` words drawn
// from `vocab` (index skewed towards low values so some terms are frequent).
// Returns the number of files written.
int WriteSyntheticCorpus(const fs::path& dir, std::mt19937& rng, int num_docs,
                         int max_words, const std::vector<std::string>& vocab) {
  const int last = static_cast<int>(vocab.size()) - 1;
  for (int d = 0; d < num_docs; ++d) {
    std::ofstream out(dir / ("doc" + std::to_string(1000 + d) + ".txt"),
                      std::ios::binary);
    const int words = RandomInt(rng, 1, max_words);
    for (int w = 0; w < words; ++w) {
      const int a = RandomInt(rng, 0, last);
      const int b = RandomInt(rng, 0, last);
      out << vocab[std::min(a, b)] << (w % 12 == 11 ? "\r\n" : " ");
    }
  }
  return num_docs;
}

// Verifies the harness helpers themselves.
void CheckHarness() {
  for (uint32_t seed : kSeeds) {
    std::mt19937 rng = MakeRng(seed);
    TempDir dir("harness");
    const std::vector<std::string> vocab = MakeVocabulary(50);
    const int docs = RandomInt(rng, 3, 10);
    WriteSyntheticCorpus(dir.path(), rng, docs, 40, vocab);
    int count = 0;
    for (const auto& entry : fs::directory_iterator(dir.path())) {
      if (entry.path().extension() == ".txt") ++count;
    }
    Check(count == docs, "synthetic corpus file count");
  }
}

}  // namespace

int main() {
  CheckHarness();
  if (FailureCount() != 0) {
    Log("index_stress_test: " + std::to_string(FailureCount()) + " failure(s)");
    return 1;
  }
  Log("index_stress_test: OK");
  return 0;
}
