#include "api_server.h"

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

// Stubs: routes are registered in later tasks.
void ApiServer::RegisterIndex() {}
void ApiServer::RegisterSearch() {}
void ApiServer::RegisterStats() {}
void ApiServer::RegisterWebSocket() {}
void ApiServer::RegisterErrorHandling() {}

}  // namespace search_engine
