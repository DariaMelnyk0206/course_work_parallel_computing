#include "api_server.h"

#include <utility>

namespace search_engine {

ApiServer::ApiServer(Config config, std::shared_ptr<SearchService> service)
    : config_(std::move(config)), service_(std::move(service)) {}

// Stub: the real implementation installs routes, the request_pool task queue
// and server settings, then blocks in listen().
bool ApiServer::Listen() { return false; }

// Stubs: routes are registered in later tasks.
void ApiServer::RegisterHealth() {}
void ApiServer::RegisterIndex() {}
void ApiServer::RegisterSearch() {}
void ApiServer::RegisterStats() {}
void ApiServer::RegisterWebSocket() {}
void ApiServer::RegisterErrorHandling() {}

}  // namespace search_engine
