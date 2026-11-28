#include "api_server.h"

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
  RegisterStats();
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

void ApiServer::RegisterSearch() {}

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
void ApiServer::RegisterErrorHandling() {}

}  // namespace search_engine
