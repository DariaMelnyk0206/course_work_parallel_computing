#include <unistd.h>

#include <csignal>
#include <memory>
#include <thread>

#include "api_server.h"
#include "config.h"
#include "logger.h"
#include "search_service.h"

int main() {
  using search_engine::Logger;

  // Block termination signals before any thread is created so that every
  // thread inherits the mask and only the dedicated waiter receives them.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);

  search_engine::Config config = search_engine::Config::FromEnvironment();
  auto service = std::make_shared<search_engine::SearchService>(config);
  search_engine::ApiServer server(config, service);

  std::jthread signal_waiter([&signals, &server](std::stop_token) {
    int received = 0;
    sigwait(&signals, &received);
    Logger::Instance().Info("Signal received, shutting down");
    server.Stop();
  });

  const bool ok = server.Listen();
  // Wake the waiter if Listen() returned without a signal (e.g. bind error).
  kill(getpid(), SIGTERM);  // Blocked everywhere but in sigwait...
  signal_waiter.join();
  if (!ok) Logger::Instance().Error("Server failed to start or stopped");
  return ok ? 0 : 1;
}
