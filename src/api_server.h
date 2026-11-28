#ifndef SEARCH_ENGINE_API_SERVER_H_
#define SEARCH_ENGINE_API_SERVER_H_

#include <memory>

#include "config.h"
#include "httplib.h"
#include "search_service.h"

namespace search_engine {

// Registers REST and WebSocket routes on an httplib::Server and applies the
// server settings from PD Section 10.2 (keep-alive, payload limit, WS read
// timeout). The request_pool is not a field: httplib owns it through the
// new_task_queue factory installed in the constructor (see task 3.1).
class ApiServer {
 public:
  ApiServer(Config config, std::shared_ptr<SearchService> service);

  // Blocks until the server stops (Requirement 5.1).
  bool Listen();

  // Stops the server; makes Listen() return. Safe to call from any thread.
  void Stop();

 private:
  void RegisterHealth();         // GET /api/health (Requirement 14).
  void RegisterIndex();          // POST /api/index (Requirement 9, 10).
  void RegisterSearch();         // POST /api/search (Requirement 17).
  void RegisterStats();          // GET /api/stats (Requirement 13).
  void RegisterWebSocket();      // WS /ws/search (Requirement 19).
  void RegisterErrorHandling();  // 404/413/500 JSON bodies (Requirement 20).

  Config config_;
  std::shared_ptr<SearchService> service_;
  httplib::Server server_;
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_API_SERVER_H_
