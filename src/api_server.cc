#include "api_server.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>

#include "logger.h"
#include "nlohmann/json.hpp"
#include "thread_pool.h"

namespace search_engine {

namespace {

constexpr size_t kPayloadMaxBytes = 1024 * 1024;  // 1 MB (Requirement 20.6).
constexpr size_t kKeepAliveMaxCount = 100;
constexpr time_t kKeepAliveTimeoutSec = 2;

// Replaces invalid UTF-8 instead of throwing (PD Section 6.2).
std::string DumpJson(const nlohmann::json& body) {
  return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool IsBlank(const std::string& s) {
  return s.find_first_not_of(" \t\n\r\f\v") == std::string::npos;
}

void SetError(httplib::Response& res, int status, const std::string& message) {
  res.status = status;
  res.set_content(DumpJson({{"error", message}}), "application/json");
}

}  // namespace

ApiServer::ApiServer(Config config, std::shared_ptr<SearchService> service)
    : config_(std::move(config)), service_(std::move(service)) {
  // httplib stores the factory result in a std::unique_ptr<TaskQueue> inside
  // Server::listen, calls shutdown() after the server stops and deletes the
  // pool. ApiServer therefore must not own the request_pool itself.
  server_.new_task_queue =
      [threads = config_.request_threads,
       queue = config_.request_queue]() -> httplib::TaskQueue* {
    return new ThreadPool(static_cast<size_t>(threads),
                          static_cast<size_t>(queue));
  };
  server_.set_keep_alive_timeout(kKeepAliveTimeoutSec);
  server_.set_keep_alive_max_count(kKeepAliveMaxCount);
  server_.set_payload_max_length(kPayloadMaxBytes);
  if (!server_.set_mount_point("/", config_.web_dir)) {
    Logger::Instance().Warn("Web directory not found, static files disabled: " +
                            config_.web_dir);
  }
  RegisterHealth();
  RegisterIndex();
  RegisterSearch();
  RegisterStats();
  RegisterErrorHandling();
}

bool ApiServer::Listen() {
  Logger::Instance().Info("Listening on 0.0.0.0:" +
                          std::to_string(config_.port));
  return server_.listen("0.0.0.0", config_.port);
}

void ApiServer::Stop() { server_.stop(); }

void ApiServer::RegisterHealth() {
  server_.Get("/api/health",
              [this](const httplib::Request&, httplib::Response& res) {
                nlohmann::json body = {{"status", "ok"},
                                       {"ollama", service_->OllamaAvailable()}};
                res.set_content(DumpJson(body), "application/json");
              });
}

void ApiServer::RegisterIndex() {
  server_.Post("/api/index", [this](const httplib::Request& req,
                                    httplib::Response& res) {
    try {
      nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
      if (!body.is_object()) {
        SetError(res, 400, "Invalid JSON");
        return;
      }
      auto it = body.find("threads");
      // is_number_integer() is false for booleans, floats and strings.
      if (it == body.end() ||
          !(it->is_number_integer() || it->is_number_unsigned())) {
        SetError(res, 400, "Field 'threads' must be an integer");
        return;
      }
      // Range check on a 64-bit value before narrowing to int.
      const int64_t max_threads = config_.max_index_threads;
      const bool in_range =
          it->is_number_unsigned()
              ? it->get<uint64_t>() <= static_cast<uint64_t>(max_threads)
              : it->get<int64_t>() <= max_threads;
      if (!in_range || it->get<int64_t>() < 1) {
        SetError(res, 400,
                 "Field 'threads' must be between 1 and " +
                     std::to_string(config_.max_index_threads));
        return;
      }
      const int threads = static_cast<int>(it->get<int64_t>());

      uint32_t documents = 0;
      int64_t time_ms = 0;
      if (service_->BuildIndex(threads, &documents, &time_ms) ==
          IndexOutcome::kAlreadyInProgress) {
        SetError(res, 409, "Indexing already in progress");
        return;
      }
      nlohmann::json out = {{"status", "ok"},
                            {"documents_indexed", documents},
                            {"time_ms", time_ms}};
      res.set_content(DumpJson(out), "application/json");
    } catch (const std::exception& e) {
      Logger::Instance().Error(std::string("POST /api/index failed: ") +
                               e.what());
      SetError(res, 500, "Internal server error");
    } catch (...) {
      SetError(res, 500, "Internal server error");
    }
  });
}

void ApiServer::RegisterSearch() {
  server_.Post("/api/search", [this](const httplib::Request& req,
                                     httplib::Response& res) {
    try {
      const auto start = std::chrono::steady_clock::now();
      nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
      if (!body.is_object()) {
        SetError(res, 400, "Invalid JSON");
        return;
      }
      auto query_it = body.find("query");
      if (query_it == body.end() || !query_it->is_string() ||
          IsBlank(query_it->get_ref<const std::string&>())) {
        SetError(res, 400, "Field 'query' must be a non-empty string");
        return;
      }
      auto mode_it = body.find("mode");
      if (mode_it == body.end() || !mode_it->is_string() ||
          mode_it->get_ref<const std::string&>() != "keyword") {
        SetError(res, 400, "Field 'mode' must be 'keyword'");
        return;
      }

      // The original query text goes to the LLM unchanged (PD Section 8.1).
      const QueryExpansion expansion =
          service_->ExpandQuery(query_it->get<std::string>());
      const KeywordSearchOutcome outcome =
          service_->RunKeywordSearch(expansion);

      nlohmann::json results = nlohmann::json::array();
      for (const SearchResult& r : outcome.results) {
        results.push_back(
            {{"doc_id", r.doc_id}, {"score", r.score}, {"snippet", r.snippet}});
      }
      const int64_t total_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count();
      nlohmann::json out = {{"results", std::move(results)},
                            {"time_ms", total_ms}};
      res.set_content(DumpJson(out), "application/json");
    } catch (const std::exception& e) {
      Logger::Instance().Error(std::string("POST /api/search failed: ") +
                               e.what());
      SetError(res, 500, "Internal server error");
    } catch (...) {
      SetError(res, 500, "Internal server error");
    }
  });
}

void ApiServer::RegisterStats() {
  server_.Get("/api/stats", [this](const httplib::Request&,
                                   httplib::Response& res) {
    try {
      const IndexStats stats = service_->Stats();
      nlohmann::json out = {{"documents", stats.documents},
                            {"index_build_time_ms", stats.index_build_time_ms},
                            {"threads_used", stats.threads_used}};
      res.set_content(DumpJson(out), "application/json");
    } catch (const std::exception& e) {
      Logger::Instance().Error(std::string("GET /api/stats failed: ") +
                               e.what());
      SetError(res, 500, "Internal server error");
    } catch (...) {
      SetError(res, 500, "Internal server error");
    }
  });
}

void ApiServer::RegisterWebSocket() {}
void ApiServer::RegisterErrorHandling() {
  // Fills a JSON body for any error without one (404, 405, 413, ...). Bodies
  // set by route handlers are left untouched.
  server_.set_error_handler([](const httplib::Request&,
                               httplib::Response& res) {
    if (!res.body.empty()) return;
    switch (res.status) {
      case 404:
        SetError(res, 404, "Not found");
        break;
      case 405:
        SetError(res, 405, "Method not allowed");
        break;
      case 413:
        SetError(res, 413, "Payload too large");
        break;
      case 500:
        SetError(res, 500, "Internal server error");
        break;
      default:
        SetError(res, res.status,
                 res.status >= 500 ? "Internal server error" : "Bad request");
        break;
    }
  });
  server_.set_exception_handler([](const httplib::Request& req,
                                   httplib::Response& res,
                                   std::exception_ptr ep) {
    try {
      if (ep) std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      Logger::Instance().Error(req.method + " " + req.path +
                               " failed: " + e.what());
    } catch (...) {
      Logger::Instance().Error(req.method + " " + req.path +
                               " failed: unknown exception");
    }
    SetError(res, 500, "Internal server error");
  });
}

}  // namespace search_engine
