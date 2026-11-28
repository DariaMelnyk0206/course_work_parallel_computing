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
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "index_builder.h"
#include "inverted_index.h"
#include "ranker.h"
#include "search_service.h"
#include "striped_hash_table.h"
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

// Property 3: Submit delivers the result or the exception (Requirements 4.8,
// 4.9). For pure functions that either return a value or throw, future.get()
// reproduces exactly what a direct synchronous call produces.
void CheckSubmitResultAndException() {
  using search_engine::ThreadPool;
  for (uint32_t seed : kSeeds) {
    std::mt19937 rng = MakeRng(seed);
    const size_t workers = static_cast<size_t>(RandomInt(rng, 1, 8));
    const int n = RandomInt(rng, 20, 200);
    ThreadPool pool(workers, 0);

    // Pure function: throws std::invalid_argument for odd inputs, otherwise
    // returns a value derived from the input.
    auto pure = [](int x) {
      if (x % 2 != 0) throw std::invalid_argument("odd:" + std::to_string(x));
      return x * 3 + 1;
    };

    std::vector<int> inputs;
    std::vector<std::future<int>> futures;
    for (int i = 0; i < n; ++i) {
      const int x = RandomInt(rng, -1000, 1000);
      inputs.push_back(x);
      futures.push_back(pool.Submit([pure, x] { return pure(x); }));
    }
    // Void task whose future must become ready without a value.
    std::atomic<int> void_ran{0};
    std::future<void> void_future = pool.Submit([&] { ++void_ran; });
    // Task throwing a type outside the std::exception hierarchy.
    std::future<int> odd_future = pool.Submit([]() -> int { throw 42; });

    for (int i = 0; i < n; ++i) {
      const int x = inputs[i];
      bool expected_threw = false;
      std::string expected_what;
      int expected_value = 0;
      try {
        expected_value = pure(x);
      } catch (const std::invalid_argument& e) {
        expected_threw = true;
        expected_what = e.what();
      }
      try {
        const int got = futures[i].get();
        Check(!expected_threw && got == expected_value,
              "submit: future returns the same value as a direct call");
      } catch (const std::invalid_argument& e) {
        Check(expected_threw && expected_what == e.what(),
              "submit: future rethrows the same exception as a direct call");
      } catch (...) {
        Check(false, "submit: unexpected exception type from future");
      }
    }

    void_future.get();
    Check(void_ran.load() == 1, "submit: void task ran exactly once");
    try {
      odd_future.get();
      Check(false, "submit: non-std exception must propagate");
    } catch (int v) {
      Check(v == 42, "submit: non-std exception value preserved");
    } catch (...) {
      Check(false, "submit: non-std exception type preserved");
    }

    // The pool keeps working after tasks threw.
    Check(pool.Submit([] { return 7; }).get() == 7,
          "submit: pool usable after throwing tasks");

    // Submit to a stopped pool is rejected with std::runtime_error.
    pool.shutdown();
    bool rejected = false;
    try {
      pool.Submit([] { return 1; });
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    Check(rejected, "submit: stopped pool rejects with runtime_error");
  }
}

// Property 4: an exception from an enqueue() task does not stop the pool
// (Requirement 4.10). Any subset of tasks throws (std and non-std types); all
// non-throwing tasks still run, and the pool keeps accepting new tasks.
void CheckEnqueueExceptionsKeepPoolAlive() {
  using search_engine::ThreadPool;
  for (uint32_t seed : kSeeds) {
    std::mt19937 rng = MakeRng(seed);
    const size_t workers = static_cast<size_t>(RandomInt(rng, 1, 8));
    const int n = RandomInt(rng, 20, 300);
    ThreadPool pool(workers, 0);

    std::atomic<int> ok_ran{0};
    std::atomic<int> throwing_ran{0};
    int expected_ok = 0;
    bool all_accepted = true;
    for (int i = 0; i < n; ++i) {
      const int kind = RandomInt(rng, 0, 3);  // 0,1: ok; 2: std; 3: non-std.
      if (kind <= 1) {
        ++expected_ok;
        all_accepted &= pool.enqueue([&] { ++ok_ran; });
      } else if (kind == 2) {
        all_accepted &= pool.enqueue([&] {
          ++throwing_ran;
          throw std::runtime_error("boom");
        });
      } else {
        all_accepted &= pool.enqueue([&] {
          ++throwing_ran;
          throw 7;
        });
      }
    }
    Check(all_accepted, "enqueue-exc: all tasks accepted");

    // Pool still accepts and runs new tasks once the batch has been handled.
    std::promise<void> done;
    std::future<void> done_future = done.get_future();
    // Wait for the batch: tasks are FIFO, but several workers run concurrently,
    // so poll the counters before submitting the probe.
    while (ok_ran.load() + throwing_ran.load() < n) std::this_thread::yield();
    Check(pool.enqueue([&] { done.set_value(); }),
          "enqueue-exc: enqueue after exceptions returns true");
    done_future.get();

    // Every worker must still be alive: block `workers` tasks on a barrier
    // that only opens when all of them run simultaneously.
    std::atomic<size_t> arrived{0};
    std::promise<void> all_here;
    std::shared_future<void> all_here_future = all_here.get_future().share();
    std::atomic<size_t> finished{0};
    for (size_t i = 0; i < workers; ++i) {
      Check(pool.enqueue([&] {
        if (++arrived == workers) all_here.set_value();
        all_here_future.wait();
        ++finished;
      }),
            "enqueue-exc: barrier task accepted");
    }
    pool.shutdown();
    Check(finished.load() == workers,
          "enqueue-exc: all workers survived throwing tasks");
    Check(ok_ran.load() == expected_ok,
          "enqueue-exc: every non-throwing task ran");
  }
}

// Deterministic offset for a (document, term) pair used by the insert checks.
// It differs per pair, so a lost or swapped offset is detected.
uint32_t OffsetFor(int doc, int term) {
  return static_cast<uint32_t>(doc) * 1009 + static_cast<uint32_t>(term) * 7 +
         1;
}

// Property 5: concurrent InsertBatch calls neither lose nor duplicate postings
// (Requirements 7.1, 7.6, 7.8, 7.11). Several writer threads insert documents
// (each with a set of unique terms) while reader threads call Lookup. A small
// bucket/stripe count forces collisions inside buckets and stripes. After all
// writers finish, Lookup(t) must hold exactly the doc_ids that inserted t,
// each once; readers must never see a duplicate or an unknown doc_id.
void CheckConcurrentInsertBatch() {
  using search_engine::Posting;
  using search_engine::StripedHashTable;
  using search_engine::StripedInsertItem;
  for (uint32_t seed : kSeeds) {
    std::mt19937 rng = MakeRng(seed);
    const size_t buckets = size_t{1} << RandomInt(rng, 4, 8);
    const size_t stripes = size_t{1} << RandomInt(rng, 0, 4);
    const int num_writers = RandomInt(rng, 2, 8);
    const int num_readers = RandomInt(rng, 1, 4);
    const int num_docs = RandomInt(rng, 50, 300);
    const std::vector<std::string> vocab =
        MakeVocabulary(RandomInt(rng, 20, 120));
    const int last = static_cast<int>(vocab.size()) - 1;

    // Generate unique terms per document and the expected postings.
    std::vector<std::vector<int>> doc_terms(num_docs);
    std::map<std::string, std::vector<Posting>> expected;
    for (int d = 0; d < num_docs; ++d) {
      std::unordered_set<int> chosen;
      const int count = RandomInt(rng, 1, 30);
      for (int i = 0; i < count; ++i) chosen.insert(RandomInt(rng, 0, last));
      doc_terms[d].assign(chosen.begin(), chosen.end());
      for (int t : doc_terms[d]) {
        expected[vocab[t]].push_back(
            {static_cast<uint32_t>(d), OffsetFor(d, t)});
      }
    }

    StripedHashTable table(buckets, stripes);
    std::atomic<int> next_doc{0};
    std::atomic<bool> writers_done{false};
    std::atomic<int> reader_violations{0};

    std::vector<std::jthread> threads;
    for (int r = 0; r < num_readers; ++r) {
      threads.emplace_back([&, r] {
        std::mt19937 local(seed * 31 + static_cast<uint32_t>(r));
        auto check_once = [&] {
          const std::string& term = vocab[RandomInt(local, 0, last)];
          const std::vector<Posting> got =
              table.Lookup(term, StripedHashTable::Fnv1aHash64(term));
          const auto it = expected.find(term);
          std::unordered_set<uint32_t> seen;
          for (const Posting& posting : got) {
            const uint32_t id = posting.doc_id;
            const bool known = it != expected.end() &&
                               std::find(it->second.begin(), it->second.end(),
                                         posting) != it->second.end();
            if (!known || !seen.insert(id).second) ++reader_violations;
          }
        };
        while (!writers_done.load()) check_once();
        check_once();
      });
    }
    {
      std::vector<std::jthread> writers;
      for (int w = 0; w < num_writers; ++w) {
        writers.emplace_back([&] {
          for (;;) {
            const int d = next_doc.fetch_add(1);
            if (d >= num_docs) return;
            std::map<size_t, std::vector<StripedInsertItem>> by_stripe;
            for (int t : doc_terms[d]) {
              const uint64_t h = StripedHashTable::Fnv1aHash64(vocab[t]);
              by_stripe[table.StripeFor(h)].push_back(
                  {vocab[t], h, static_cast<uint32_t>(d), OffsetFor(d, t)});
            }
            for (const auto& [stripe, items] : by_stripe) {
              table.InsertBatch(stripe, items);
            }
          }
        });
      }
    }  // Writers joined here.
    writers_done = true;
    threads.clear();  // Joins readers.

    Check(reader_violations.load() == 0,
          "insert: readers saw a duplicate or unknown doc_id");
    for (auto& [term, pairs] : expected) {
      std::vector<Posting> got =
          table.Lookup(term, StripedHashTable::Fnv1aHash64(term));
      std::sort(got.begin(), got.end());
      std::sort(pairs.begin(), pairs.end());
      Check(got == pairs,
            "insert: postings equal inserted (doc_id, offset) pairs exactly");
    }
    Check(table.UniqueTermCount() == expected.size(),
          "insert: unique term count matches");
    const std::string absent = "zzzabsent";
    Check(table.Lookup(absent, StripedHashTable::Fnv1aHash64(absent)).empty(),
          "insert: absent term has empty postings");
  }
}

// Property 6: Lookup returns an independent copy (Requirements 7.7, 7.9). A
// vector obtained from Lookup(t) stays unchanged while other threads insert
// more postings and perform further Lookups. Mutating the returned vector
// does not affect what later Lookups return.
void CheckLookupReturnsIndependentCopy() {
  using search_engine::Posting;
  using search_engine::StripedHashTable;
  using search_engine::StripedInsertItem;
  for (uint32_t seed : kSeeds) {
    std::mt19937 rng = MakeRng(seed);
    const size_t buckets = size_t{1} << RandomInt(rng, 3, 7);
    const size_t stripes = size_t{1} << RandomInt(rng, 0, 3);
    const int num_terms = RandomInt(rng, 3, 20);
    const int initial_docs = RandomInt(rng, 5, 40);
    const int extra_docs = RandomInt(rng, 50, 300);
    const int num_writers = RandomInt(rng, 2, 6);
    const std::vector<std::string> vocab = MakeVocabulary(num_terms);

    StripedHashTable table(buckets, stripes);
    auto insert_doc = [&](uint32_t d) {
      for (const std::string& term : vocab) {
        const uint64_t h = StripedHashTable::Fnv1aHash64(term);
        table.InsertBatch(table.StripeFor(h), {{term, h, d, d}});
      }
    };
    // Documents with ids [0, initial_docs) are present before snapshots.
    for (int d = 0; d < initial_docs; ++d) insert_doc(static_cast<uint32_t>(d));

    // Snapshots taken now, plus reference copies of their contents.
    std::vector<std::vector<Posting>> snapshots;
    std::vector<std::vector<Posting>> references;
    for (const std::string& term : vocab) {
      snapshots.push_back(
          table.Lookup(term, StripedHashTable::Fnv1aHash64(term)));
      references.push_back(snapshots.back());
    }

    // Writers add further documents; a reader mutates its own copies and
    // checks that fresh Lookups never reflect those mutations.
    std::atomic<int> next_doc{initial_docs};
    std::atomic<bool> writers_done{false};
    std::atomic<int> mutation_leaks{0};
    {
      std::vector<std::jthread> threads;
      threads.emplace_back([&] {
        auto probe = [&] {
          for (const std::string& term : vocab) {
            const uint64_t h = StripedHashTable::Fnv1aHash64(term);
            std::vector<Posting> mine = table.Lookup(term, h);
            const std::vector<Posting> before = mine;
            const Posting sentinel{0xFFFFFFFFu, 0xFFFFFFFFu};
            std::fill(mine.begin(), mine.end(), sentinel);
            mine.push_back(sentinel);
            std::vector<Posting> again = table.Lookup(term, h);
            // `again` may have grown, but must never contain the sentinel.
            if (std::find(again.begin(), again.end(), sentinel) !=
                again.end()) {
              ++mutation_leaks;
            }
            if (again.size() < before.size()) ++mutation_leaks;
          }
        };
        while (!writers_done.load()) probe();
        probe();
      });
      std::vector<std::jthread> writers;
      for (int w = 0; w < num_writers; ++w) {
        writers.emplace_back([&] {
          for (;;) {
            const int d = next_doc.fetch_add(1);
            if (d >= initial_docs + extra_docs) return;
            insert_doc(static_cast<uint32_t>(d));
          }
        });
      }
      writers.clear();  // Joins writers.
      writers_done = true;
    }  // Reader joined here.

    Check(mutation_leaks.load() == 0,
          "copy: mutating a returned vector leaked into the table");
    for (size_t i = 0; i < vocab.size(); ++i) {
      Check(snapshots[i] == references[i],
            "copy: earlier Lookup result unchanged by later inserts");
      const uint64_t h = StripedHashTable::Fnv1aHash64(vocab[i]);
      std::vector<Posting> fresh = table.Lookup(vocab[i], h);
      Check(fresh.size() == static_cast<size_t>(initial_docs + extra_docs),
            "copy: fresh Lookup sees all inserted postings");
      // Mutating a snapshot must not change the table.
      std::fill(snapshots[i].begin(), snapshots[i].end(),
                Posting{0xFFFFFFFFu, 0xFFFFFFFFu});
      Check(table.Lookup(vocab[i], h) == fresh,
            "copy: mutating a snapshot does not change later Lookups");
    }
  }
}

// Independent first-occurrence offset: byte position of the first whole-token
// match of `term` in `text` (found with std::string::find, then checking that
// both neighbours are not ASCII letters/digits), or npos.
size_t FirstTokenOffset(const std::string& text, const std::string& term) {
  auto is_alnum = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
  };
  for (size_t pos = text.find(term); pos != std::string::npos;
       pos = text.find(term, pos + 1)) {
    const bool left_ok = pos == 0 || !is_alnum(text[pos - 1]);
    const size_t end = pos + term.size();
    const bool right_ok = end >= text.size() || !is_alnum(text[end]);
    if (left_ok && right_ok) return pos;
  }
  return std::string::npos;
}

// For every vocabulary term, the postings in `index` must be exactly the
// (doc_id, first-occurrence offset) pairs computed from the file texts.
void CheckOffsetsAgainstText(const fs::path& dir,
                             const std::vector<std::string>& vocab,
                             const search_engine::InvertedIndex& index) {
  using search_engine::Posting;
  using search_engine::StripedHashTable;
  std::map<std::string, std::vector<Posting>> expected;
  for (uint32_t d = 0; d < index.DocumentCount(); ++d) {
    std::ifstream in(dir / index.Documents().Get(d).name, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    for (const std::string& term : vocab) {
      const size_t pos = FirstTokenOffset(text, term);
      if (pos != std::string::npos) {
        expected[term].push_back({d, static_cast<uint32_t>(pos)});
      }
    }
  }
  for (const std::string& term : vocab) {
    std::vector<Posting> got =
        index.Table().Lookup(term, StripedHashTable::Fnv1aHash64(term));
    std::vector<Posting> want = expected[term];
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
    Check(got == want,
          "offsets: posting offsets equal independent first-occurrence "
          "positions in the file text");
  }
}

// Feature: llm-search-engine, Property 7: index content does not depend on
// the number of indexing threads (Requirements 9.4, 28.4). A synthetic corpus
// is built through IndexBuilder::Build with 1, 2, 4 and 8 threads; document
// count, unique term count, document names and snippets, and the sorted
// postings of every term must be identical across all builds, and
// Ranker::Rank must return identical results for a fixed set of queries.
void CheckIndexDeterminismAcrossThreadCounts() {
  using search_engine::IndexBuilder;
  using search_engine::IndexBuildResult;
  using search_engine::Posting;
  using search_engine::Ranker;
  using search_engine::StripedHashTable;
  using search_engine::WeightedTerm;
  constexpr int kThreadCounts[] = {1, 2, 4, 8};
  for (uint32_t seed : kSeeds) {
    std::mt19937 rng = MakeRng(seed);
    TempDir dir("determinism");
    const std::vector<std::string> vocab =
        MakeVocabulary(RandomInt(rng, 20, 150));
    const int num_docs = RandomInt(rng, 20, 120);
    WriteSyntheticCorpus(dir.path(), rng, num_docs, 80, vocab);

    std::vector<IndexBuildResult> builds;
    for (int threads : kThreadCounts) {
      builds.push_back(IndexBuilder::Build(dir.path().string(), threads));
    }

    const IndexBuildResult& base = builds[0];
    Check(base.index != nullptr &&
              base.documents_indexed == static_cast<uint32_t>(num_docs),
          "determinism: baseline indexed every document");
    if (base.index == nullptr) continue;
    CheckOffsetsAgainstText(dir.path(), vocab, *base.index);
    for (size_t b = 1; b < builds.size(); ++b) {
      if (builds[b].index != nullptr) {
        CheckOffsetsAgainstText(dir.path(), vocab, *builds[b].index);
      }
    }
    for (size_t b = 1; b < builds.size(); ++b) {
      const IndexBuildResult& other = builds[b];
      if (other.index == nullptr) {
        Check(false, "determinism: build returned no index");
        continue;
      }
      Check(other.documents_indexed == base.documents_indexed &&
                other.index->DocumentCount() == base.index->DocumentCount(),
            "determinism: document count equal across thread counts");
      Check(other.index->Table().UniqueTermCount() ==
                base.index->Table().UniqueTermCount(),
            "determinism: unique term count equal across thread counts");
      for (uint32_t d = 0; d < base.index->DocumentCount(); ++d) {
        const auto& x = base.index->Documents().Get(d);
        const auto& y = other.index->Documents().Get(d);
        Check(x.name == y.name && x.snippet == y.snippet && x.valid == y.valid,
              "determinism: document metadata equal across thread counts");
      }
      for (const std::string& term : vocab) {
        const uint64_t h = StripedHashTable::Fnv1aHash64(term);
        std::vector<Posting> a = base.index->Table().Lookup(term, h);
        std::vector<Posting> c = other.index->Table().Lookup(term, h);
        std::sort(a.begin(), a.end());
        std::sort(c.begin(), c.end());
        Check(a == c, "determinism: sorted postings equal across threads");
      }

      // Search results (task 10.2): fixed weighted queries must rank
      // identically (doc_id, score, snippet, order) on both indexes.
      std::mt19937 qrng(seed * 17 + static_cast<uint32_t>(b));
      for (int q = 0; q < 10; ++q) {
        std::vector<WeightedTerm> terms;
        const int count = RandomInt(qrng, 1, 6);
        for (int i = 0; i < count; ++i) {
          const std::string& t =
              vocab[RandomInt(qrng, 0, static_cast<int>(vocab.size()) - 1)];
          terms.push_back({t, StripedHashTable::Fnv1aHash64(t),
                           RandomInt(qrng, 0, 1) == 0 ? 2.0 : 1.0});
        }
        if (q % 3 == 0) {  // Term absent from the index (DF = 0).
          const std::string absent = "zzzabsent";
          terms.push_back({absent, StripedHashTable::Fnv1aHash64(absent), 1.0});
        }
        const int top_k = RandomInt(qrng, 1, 30);
        const auto r1 = Ranker::Rank(terms, *base.index, top_k);
        const auto r2 = Ranker::Rank(terms, *other.index, top_k);
        bool same = r1.size() == r2.size();
        for (size_t i = 0; same && i < r1.size(); ++i) {
          same = r1[i].doc_index == r2[i].doc_index &&
                 r1[i].score == r2[i].score &&
                 r1[i].anchor_offset == r2[i].anchor_offset &&
                 r1[i].anchor_term == r2[i].anchor_term;
        }
        Check(same, "determinism: search results equal across thread counts");
      }
    }
  }
}

// Collapses whitespace runs into one space and trims both ends (independent
// reimplementation used to locate a snippet body inside the file text).
std::string CollapseWhitespace(const std::string& text) {
  std::string out;
  bool pending = false;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      pending = !out.empty();
    } else {
      if (pending) out.push_back(' ');
      pending = false;
      out.push_back(c);
    }
  }
  return out;
}

// Feature: llm-search-engine, Property 8: a KWIC snippet contains the anchor
// term, fits into 160 bytes and has "..." exactly on truncated edges; a
// corrupted anchor offset or a missing file gives the static snippet
// (Requirements 12.6, 12.8, 12.9, 12.10, 28.9, 28.10). Also checks that
// Rank + AttachSnippets give identical results for 1, 2, 4 and 8 build
// threads, including anchors (Property 7, Requirement 28.4).
void CheckKwicSnippetsAndFallback() {
  using search_engine::IndexBuilder;
  using search_engine::IndexBuildResult;
  using search_engine::RankedDocument;
  using search_engine::Ranker;
  using search_engine::SearchResult;
  using search_engine::SearchService;
  using search_engine::StripedHashTable;
  using search_engine::WeightedTerm;
  constexpr int kThreadCounts[] = {1, 2, 4, 8};
  const std::string kDots = "...";
  for (uint32_t seed : kSeeds) {
    std::mt19937 rng = MakeRng(seed);
    TempDir dir("kwic");
    const std::vector<std::string> vocab =
        MakeVocabulary(RandomInt(rng, 20, 80));
    const int num_docs = RandomInt(rng, 10, 40);
    WriteSyntheticCorpus(dir.path(), rng, num_docs, 80, vocab);
    const std::string dataset = dir.path().string();

    std::vector<WeightedTerm> terms;
    const int count = RandomInt(rng, 1, 4);
    for (int i = 0; i < count; ++i) {
      const std::string& t =
          i == 0 ? vocab[0]
                 : vocab[RandomInt(rng, 0, static_cast<int>(vocab.size()) - 1)];
      if (std::any_of(terms.begin(), terms.end(),
                      [&](const WeightedTerm& w) { return w.term == t; })) {
        continue;
      }
      terms.push_back({t, StripedHashTable::Fnv1aHash64(t),
                       RandomInt(rng, 0, 1) == 0 ? 2.0 : 1.0});
    }

    std::vector<IndexBuildResult> builds;
    std::vector<std::vector<RankedDocument>> ranked;
    std::vector<std::vector<SearchResult>> attached;
    for (int threads : kThreadCounts) {
      builds.push_back(IndexBuilder::Build(dataset, threads));
      const IndexBuildResult& build = builds.back();
      if (build.index == nullptr) {
        Check(false, "kwic: build returned no index");
        ranked.emplace_back();
        attached.emplace_back();
        continue;
      }
      ranked.push_back(Ranker::Rank(terms, *build.index, 10));
      attached.push_back(
          SearchService::AttachSnippets(dataset, *build.index, ranked.back()));
    }
    if (builds[0].index == nullptr) continue;
    const search_engine::InvertedIndex& index = *builds[0].index;
    const std::vector<RankedDocument>& base = ranked[0];
    const std::vector<SearchResult>& base_results = attached[0];
    Check(!base.empty() && base.size() == base_results.size(),
          "kwic: query has results, one snippet per ranked document");
    if (base.empty() || base.size() != base_results.size()) continue;

    // Property 7 part: ranking, anchors and snippets equal for all threads.
    for (size_t b = 1; b < builds.size(); ++b) {
      bool same = ranked[b].size() == base.size() &&
                  attached[b].size() == base_results.size();
      for (size_t i = 0; same && i < base.size(); ++i) {
        same = ranked[b][i].doc_index == base[i].doc_index &&
               ranked[b][i].score == base[i].score &&
               ranked[b][i].anchor_offset == base[i].anchor_offset &&
               ranked[b][i].anchor_term == base[i].anchor_term &&
               attached[b][i].doc_id == base_results[i].doc_id &&
               attached[b][i].snippet == base_results[i].snippet;
      }
      Check(same, "kwic: ranking, anchors and snippets equal across threads");
    }

    // Property 8, valid anchors: every result (including the first).
    for (size_t i = 0; i < base.size(); ++i) {
      const auto& info = index.Documents().Get(base[i].doc_index);
      std::ifstream in(dir.path() / info.relative_path, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
      const std::string normalized = CollapseWhitespace(text);

      std::string body = base_results[i].snippet;
      Check(body.size() <= 160, "kwic: snippet longer than 160 bytes");
      const bool left = body.compare(0, 3, kDots) == 0;
      if (left) body.erase(0, 3);
      const bool right =
          body.size() >= 3 && body.compare(body.size() - 3, 3, kDots) == 0;
      if (right) body.erase(body.size() - 3);

      Check(FirstTokenOffset(body, base[i].anchor_term) != std::string::npos,
            "kwic: snippet contains the anchor term");
      const size_t pos = normalized.find(body);
      Check(pos != std::string::npos && !body.empty(),
            "kwic: snippet body is a contiguous part of the file text");
      if (pos != std::string::npos) {
        Check(left == (pos > 0), "kwic: leading ... only on a cut left edge");
        Check(right == (pos + body.size() < normalized.size()),
              "kwic: trailing ... only on a cut right edge");
      }
    }

    // Property 8, corrupted anchors: the static snippet is returned.
    auto corrupt_and_check = [&](const char* what, auto make_offset) {
      std::vector<RankedDocument> broken = base;
      for (RankedDocument& doc : broken) {
        const auto& info = index.Documents().Get(doc.doc_index);
        doc.anchor_offset =
            make_offset(doc, fs::file_size(dir.path() / info.relative_path));
      }
      const auto results =
          SearchService::AttachSnippets(dataset, index, broken);
      bool all_static = results.size() == broken.size();
      for (size_t i = 0; all_static && i < results.size(); ++i) {
        all_static = results[i].snippet ==
                     index.Documents().Get(broken[i].doc_index).snippet;
      }
      Check(all_static, std::string("kwic: static snippet for ") + what);
    };
    corrupt_and_check(
        "offset inside a token",
        [](const RankedDocument& d, uintmax_t) { return d.anchor_offset + 1; });
    corrupt_and_check("offset past end of file",
                      [](const RankedDocument&, uintmax_t size) {
                        return static_cast<uint32_t>(size + 1000);
                      });
    corrupt_and_check(
        "offset 0xFFFFFFFF",
        [](const RankedDocument&, uintmax_t) { return 0xFFFFFFFFu; });

    // Property 8, missing file: only that document falls back.
    const auto& gone = index.Documents().Get(base[0].doc_index);
    fs::remove(dir.path() / gone.relative_path);
    const auto after = SearchService::AttachSnippets(dataset, index, base);
    Check(after.size() == base.size() && after[0].snippet == gone.snippet,
          "kwic: static snippet for a deleted file");
    for (size_t i = 1; i < base.size() && i < after.size(); ++i) {
      Check(after[i].snippet == base_results[i].snippet,
            "kwic: other documents keep their KWIC snippet");
    }
  }
}

}  // namespace

int main() {
  CheckHarness();
  CheckBoundedQueueLimit();
  CheckShutdownDrainsAndJoins();
  CheckSubmitResultAndException();
  CheckEnqueueExceptionsKeepPoolAlive();
  CheckConcurrentInsertBatch();
  CheckLookupReturnsIndependentCopy();
  CheckIndexDeterminismAcrossThreadCounts();
  CheckKwicSnippetsAndFallback();
  if (FailureCount() != 0) {
    Log("index_stress_test: " + std::to_string(FailureCount()) + " failure(s)");
    return 1;
  }
  Log("index_stress_test: OK");
  return 0;
}
