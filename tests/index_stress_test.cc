// Stress-test driver for ThreadSanitizer runs (Requirement 28). This file
// holds the shared harness: seeded generators, a synthetic corpus in a
// temporary directory, and a failure counter that determines the exit code.
// The individual property checks are added by later tasks.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "thread_pool.h"

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

// Property 1: bounded queue limit (Requirements 4.3, 4.4, 4.5). With every
// worker blocked and C tasks queued, the next enqueue() returns false at once;
// an unbounded pool accepts every task until shutdown().
void CheckBoundedQueueLimit() {
  using search_engine::ThreadPool;
  for (uint32_t seed : kSeeds) {
    std::mt19937 rng = MakeRng(seed);
    const size_t workers = static_cast<size_t>(RandomInt(rng, 1, 4));
    const size_t capacity = static_cast<size_t>(RandomInt(rng, 1, 16));

    std::promise<void> gate;
    std::shared_future<void> gate_future = gate.get_future().share();
    std::atomic<size_t> started{0};
    std::atomic<size_t> executed{0};
    size_t accepted = 0;
    {
      ThreadPool pool(workers, capacity);
      // Occupy every worker so that queued tasks stay queued.
      for (size_t i = 0; i < workers; ++i) {
        Check(pool.enqueue([&] {
          ++started;
          gate_future.wait();
          ++executed;
        }),
              "bounded: blocker accepted");
        ++accepted;
      }
      while (started.load() < workers) std::this_thread::yield();

      // Fill the queue to capacity: all accepted.
      for (size_t i = 0; i < capacity; ++i) {
        Check(pool.enqueue([&] { ++executed; }),
              "bounded: enqueue below capacity returns true");
        ++accepted;
      }
      // Queue is full: rejected immediately, repeatedly.
      for (int i = 0; i < 3; ++i) {
        Check(!pool.enqueue([&] { ++executed; }),
              "bounded: enqueue on full queue returns false");
      }
      gate.set_value();
      pool.shutdown();
      Check(!pool.enqueue([] {}), "bounded: enqueue after shutdown is false");
    }
    Check(executed.load() == accepted,
          "bounded: only accepted tasks executed (rejected ones dropped)");

    // Unbounded queue: always true until shutdown().
    const size_t many = static_cast<size_t>(RandomInt(rng, 200, 1000));
    std::promise<void> gate2;
    std::shared_future<void> gate2_future = gate2.get_future().share();
    std::atomic<size_t> ran{0};
    ThreadPool unbounded(workers, 0);
    bool all_accepted = true;
    for (size_t i = 0; i < many; ++i) {
      all_accepted &= unbounded.enqueue([&] {
        gate2_future.wait();
        ++ran;
      });
    }
    Check(all_accepted, "unbounded: enqueue always returns true");
    gate2.set_value();
    unbounded.shutdown();
    Check(ran.load() == many, "unbounded: all tasks executed");
    Check(!unbounded.enqueue([] {}),
          "unbounded: enqueue after shutdown is false");
  }
}

// Property 2: shutdown() drains and joins (Requirements 4.6, 4.7). After
// shutdown() returns, all N queued tasks have run, enqueue() is rejected, and
// a repeated shutdown() (as from the destructor) is a harmless no-op. Workers
// being joined is observed indirectly: the counter no longer changes after
// shutdown() returns, so no worker is still running tasks.
void CheckShutdownDrainsAndJoins() {
  using search_engine::ThreadPool;
  for (uint32_t seed : kSeeds) {
    std::mt19937 rng = MakeRng(seed);
    const size_t workers = static_cast<size_t>(RandomInt(rng, 1, 8));
    const size_t n = static_cast<size_t>(RandomInt(rng, 0, 500));
    const size_t capacity = RandomInt(rng, 0, 1) == 0 ? 0 : n + 1;

    std::atomic<size_t> executed{0};
    {
      ThreadPool pool(workers, capacity);
      bool all_accepted = true;
      for (size_t i = 0; i < n; ++i) {
        all_accepted &= pool.enqueue([&] {
          std::this_thread::yield();
          ++executed;
        });
      }
      Check(all_accepted, "shutdown: all tasks accepted before shutdown");
      pool.shutdown();
      Check(executed.load() == n, "shutdown: all queued tasks executed");
      Check(!pool.enqueue([&] { ++executed; }),
            "shutdown: enqueue after shutdown is false");
      pool.shutdown();  // Second call must not block or change anything.
      pool.shutdown();
      std::this_thread::yield();
      Check(executed.load() == n, "shutdown: no task runs after shutdown");
    }  // Destructor calls shutdown() a further time.
    Check(executed.load() == n, "shutdown: counter unchanged after destructor");
  }
}

}  // namespace

int main() {
  CheckHarness();
  CheckBoundedQueueLimit();
  CheckShutdownDrainsAndJoins();
  if (FailureCount() != 0) {
    Log("index_stress_test: " + std::to_string(FailureCount()) + " failure(s)");
    return 1;
  }
  Log("index_stress_test: OK");
  return 0;
}
