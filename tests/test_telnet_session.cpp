// Copyright (c) 2024 liudegui. MIT License.
// Tests for telsh::TelnetSession internals (IAC filter, history, auth).
//
// Note: These tests exercise the session logic without a real socket.
// We use a socketpair to simulate a telnet connection.

#include "telsh/telnet_session.hpp"

#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace telsh;

// ============================================================================
// Helper: create a socketpair and run session in a thread
// ============================================================================

struct SessionFixture {
  int client_fd = -1;
  int server_fd = -1;
  TelnetSession session;
  CommandRegistry registry;
  std::thread session_thread;

  void Setup(const SessionConfig& cfg = {}) {
    int fds[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    server_fd = fds[0];
    client_fd = fds[1];

    session.Init(server_fd, registry, cfg);
    session_thread = std::thread([this]() { session.Run(); });

    // Wait for IAC negotiations + banner to arrive
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    DrainClient();
  }

  ~SessionFixture() {
    if (client_fd >= 0) {
      close(client_fd);
    }
    if (session_thread.joinable()) {
      session.Stop();
      session_thread.join();
    }
  }

  // Send string from client side
  void ClientSend(const char* str) {
    if (client_fd >= 0 && str != nullptr) {
      write(client_fd, str, std::strlen(str));
    }
  }

  // Send raw bytes from client side
  void ClientSendRaw(const uint8_t* data, uint32_t len) {
    if (client_fd >= 0) {
      write(client_fd, data, len);
    }
  }

  // Read from client side (what server sent)
  int ClientRecv(char* buf, uint32_t size, uint32_t timeout_ms = 200) {
    // Set read timeout
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t n = read(client_fd, buf, size - 1);
    if (n > 0) {
      buf[n] = '\0';
    } else {
      buf[0] = '\0';
      n = 0;
    }
    return static_cast<int>(n);
  }

  // Drain all pending data from client
  void DrainClient() {
    char buf[1024];
    struct timeval tv = {0, 50000};  // 50ms
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (read(client_fd, buf, sizeof(buf)) > 0) {}
  }
};

// ============================================================================
// Session tests (no auth)
// ============================================================================

TEST_CASE("TelnetSession: prompt shown on connect", "[telnet_session]") {
  SessionFixture f;
  SessionConfig cfg;
  cfg.username = nullptr;
  cfg.password = nullptr;
  cfg.prompt = "test> ";
  cfg.banner = nullptr;

  int fds[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  f.session.Init(fds[0], f.registry, cfg);
  f.server_fd = fds[0];
  f.client_fd = fds[1];
  f.session_thread = std::thread([&f]() { f.session.Run(); });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  char buf[512];
  int n = f.ClientRecv(buf, sizeof(buf));
  REQUIRE(n > 0);
  // Should contain prompt (after IAC negotiations)
  REQUIRE(std::strstr(buf, "test> ") != nullptr);
}

TEST_CASE("TelnetSession: echo typed characters", "[telnet_session]") {
  SessionFixture f;
  SessionConfig cfg;
  cfg.username = nullptr;
  cfg.password = nullptr;
  cfg.prompt = "> ";
  cfg.banner = nullptr;
  f.Setup(cfg);

  f.ClientSend("abc");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  char buf[128];
  int n = f.ClientRecv(buf, sizeof(buf));
  REQUIRE(n > 0);
  REQUIRE(std::strstr(buf, "abc") != nullptr);
}

TEST_CASE("TelnetSession: execute command", "[telnet_session]") {
  SessionFixture f;

  // Register a test command
  static bool cmd_called = false;
  cmd_called = false;
  auto test_fn = [](int argc, char* argv[], void* ctx) -> int {
    (void)argc;
    (void)argv;
    (void)ctx;
    cmd_called = true;
    return 0;
  };
  f.registry.Register("ping", "test ping", test_fn);

  SessionConfig cfg;
  cfg.username = nullptr;
  cfg.password = nullptr;
  cfg.prompt = "> ";
  cfg.banner = nullptr;
  f.Setup(cfg);

  f.ClientSend("ping\r");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  REQUIRE(cmd_called);
}

TEST_CASE("TelnetSession: backspace removes character", "[telnet_session]") {
  SessionFixture f;
  SessionConfig cfg;
  cfg.username = nullptr;
  cfg.password = nullptr;
  cfg.prompt = "> ";
  cfg.banner = nullptr;
  f.Setup(cfg);

  // Type "abc", then backspace, then enter
  f.ClientSend("abc");
  f.ClientSend("\x7f");  // DEL
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  char buf[256];
  f.ClientRecv(buf, sizeof(buf));
  // Should see backspace sequence
  REQUIRE(std::strstr(buf, "\b \b") != nullptr);
}

TEST_CASE("TelnetSession: IAC sequences filtered", "[telnet_session]") {
  SessionFixture f;
  SessionConfig cfg;
  cfg.username = nullptr;
  cfg.password = nullptr;
  cfg.prompt = "> ";
  cfg.banner = nullptr;
  f.Setup(cfg);

  // Send IAC WILL SGA (should be silently consumed)
  uint8_t iac_seq[] = {255, 251, 3};  // IAC WILL SGA
  f.ClientSendRaw(iac_seq, 3);

  // Then send normal text
  f.ClientSend("ok");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  char buf[128];
  int n = f.ClientRecv(buf, sizeof(buf));
  REQUIRE(n > 0);
  REQUIRE(std::strstr(buf, "ok") != nullptr);
}

TEST_CASE("TelnetSession: exit command closes session", "[telnet_session]") {
  SessionFixture f;
  SessionConfig cfg;
  cfg.username = nullptr;
  cfg.password = nullptr;
  cfg.prompt = "> ";
  cfg.banner = nullptr;
  f.Setup(cfg);

  f.ClientSend("exit\r");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  char buf[128];
  int n = f.ClientRecv(buf, sizeof(buf));
  // Should see "Bye."
  if (n > 0) {
    REQUIRE(std::strstr(buf, "Bye") != nullptr);
  }

  // Session thread should have exited
  if (f.session_thread.joinable()) {
    f.session_thread.join();
  }
}

// ============================================================================
// Authentication tests
// ============================================================================

TEST_CASE("TelnetSession: auth success", "[telnet_session]") {
  SessionFixture f;
  SessionConfig cfg;
  cfg.username = "admin";
  cfg.password = "1234";
  cfg.prompt = "$ ";
  cfg.banner = nullptr;

  int fds[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  f.server_fd = fds[0];
  f.client_fd = fds[1];
  f.session.Init(fds[0], f.registry, cfg);
  f.session_thread = std::thread([&f]() { f.session.Run(); });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  char buf[512];
  f.ClientRecv(buf, sizeof(buf));
  // Should see "username:" prompt
  REQUIRE(std::strstr(buf, "username") != nullptr);

  // Send username
  f.ClientSend("admin\r");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  f.ClientRecv(buf, sizeof(buf));
  REQUIRE(std::strstr(buf, "password") != nullptr);

  // Send password
  f.ClientSend("1234\r");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  f.ClientRecv(buf, sizeof(buf));
  REQUIRE(std::strstr(buf, "Login OK") != nullptr);
}

TEST_CASE("TelnetSession: auth failure", "[telnet_session]") {
  SessionFixture f;
  SessionConfig cfg;
  cfg.username = "admin";
  cfg.password = "1234";
  cfg.prompt = "$ ";
  cfg.banner = nullptr;

  int fds[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  f.server_fd = fds[0];
  f.client_fd = fds[1];
  f.session.Init(fds[0], f.registry, cfg);
  f.session_thread = std::thread([&f]() { f.session.Run(); });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  char buf[512];
  f.ClientRecv(buf, sizeof(buf));

  f.ClientSend("admin\r");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  f.ClientRecv(buf, sizeof(buf));

  f.ClientSend("wrong\r");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  f.ClientRecv(buf, sizeof(buf));
  REQUIRE(std::strstr(buf, "Login failed") != nullptr);
}
