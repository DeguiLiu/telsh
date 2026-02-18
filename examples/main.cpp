// Copyright (c) 2024 liudegui. MIT License.
//
// telsh example -- demonstrates command registration and server startup.
//
// Usage:
//   ./telsh_example
//   # Then: telnet 127.0.0.1 2500

#include "telsh/telnet_server.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>

#include <thread>

// ============================================================================
// Example commands registered via TELSH_CMD macro
// ============================================================================

TELSH_CMD(hello, "Print a greeting") {
  (void)ctx;
  if (argc > 1) {
    telsh::TelnetServer::Printf("Hello, %s!\r\n", argv[1]);
  } else {
    telsh::TelnetServer::Printf("Hello, world!\r\n");
  }
  return 0;
}

TELSH_CMD(echo, "Echo arguments back") {
  (void)ctx;
  for (int i = 1; i < argc; ++i) {
    telsh::TelnetServer::Printf("%s%s", argv[i], (i < argc - 1) ? " " : "");
  }
  telsh::TelnetServer::Printf("\r\n");
  return 0;
}

TELSH_CMD(add, "Add two integers: add <a> <b>") {
  (void)ctx;
  if (argc != 3) {
    telsh::TelnetServer::Printf("Usage: add <a> <b>\r\n");
    return -1;
  }
  int a = std::atoi(argv[1]);
  int b = std::atoi(argv[2]);
  telsh::TelnetServer::Printf("%d + %d = %d\r\n", a, b, a + b);
  return 0;
}

// ============================================================================
// Example: register a member function via context pointer
// ============================================================================

struct Counter {
  int32_t value = 0;
};

static int cmd_count(int argc, char* argv[], void* ctx) {
  (void)argc;
  (void)argv;
  auto* c = static_cast<Counter*>(ctx);
  c->value++;
  telsh::TelnetServer::Printf("Counter: %d\r\n", c->value);
  return 0;
}

// ============================================================================
// Signal handling for graceful shutdown
// ============================================================================

static volatile sig_atomic_t g_quit = 0;

static void SignalHandler(int sig) {
  (void)sig;
  g_quit = 1;
}

// ============================================================================
// Main
// ============================================================================

int main() {
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  // Register commands with context
  Counter counter;
  telsh::CommandRegistry::Instance().Register("count", "Increment and show counter", cmd_count, &counter);

  // Configure server
  telsh::ServerConfig config;
  config.port = 2500;
  config.username = "admin";
  config.password = "1234";
  config.max_sessions = 4;

  // Create and start server
  telsh::TelnetServer server(telsh::CommandRegistry::Instance(), config);
  if (!server.Start()) {
    std::fprintf(stderr, "Failed to start telsh server\n");
    return 1;
  }

  std::printf("telsh server running on port %u\n", config.port);
  std::printf("  telnet 127.0.0.1 %u\n", config.port);
  std::printf("  username: admin, password: 1234\n");
  std::printf("Press Ctrl+C to stop.\n");

  // Main loop
  while (g_quit == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::printf("\nShutting down...\n");
  server.Stop();
  return 0;
}
