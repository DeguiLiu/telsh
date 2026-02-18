// Copyright (c) 2024 liudegui. MIT License.
//
// telsh::TelnetServer -- POSIX socket telnet server with fixed session pool.
//
// Design:
//   - Pure POSIX sockets (no boost)
//   - Fixed session pool (kMaxSessions = 8), zero heap allocation
//   - Each session runs in a joinable std::thread (not detached)
//   - Broadcast to all active sessions
//   - Global tel_printf() for broadcasting from anywhere
//   - Graceful shutdown: Stop() closes listen fd, stops sessions, joins threads

#pragma once

#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "telsh/command_registry.hpp"
#include "telsh/telnet_session.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <atomic>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace telsh {

// ---------------------------------------------------------------------------
// ServerConfig
// ---------------------------------------------------------------------------

struct ServerConfig {
  uint16_t port = 2500;
  const char* username = nullptr;  ///< nullptr = no auth
  const char* password = nullptr;
  const char* prompt = "telsh> ";
  const char* banner = nullptr;  ///< nullptr = use default banner
  uint32_t max_sessions = 4;
};

// ---------------------------------------------------------------------------
// TelnetServer
// ---------------------------------------------------------------------------

class TelnetServer {
 public:
  static constexpr uint32_t kMaxSessions = 8;

  explicit TelnetServer(CommandRegistry& registry, const ServerConfig& config = {})
      : registry_(&registry), config_(config) {
    OSP_ASSERT(config_.max_sessions <= kMaxSessions);
    g_instance_ = this;
  }

  ~TelnetServer() {
    Stop();
    if (g_instance_ == this) {
      g_instance_ = nullptr;
    }
  }

  // Non-copyable, non-movable
  TelnetServer(const TelnetServer&) = delete;
  TelnetServer& operator=(const TelnetServer&) = delete;
  TelnetServer(TelnetServer&&) = delete;
  TelnetServer& operator=(TelnetServer&&) = delete;

  /// Start the server.  Returns true on success.
  bool Start() {
    if (running_.load(std::memory_order_acquire)) {
      OSP_LOG_WARN("TELSH", "Server already running");
      return false;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      OSP_LOG_ERROR("TELSH", "socket() failed: %s", strerror(errno));
      return false;
    }

    int32_t opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.port);

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      OSP_LOG_ERROR("TELSH", "bind(%u) failed: %s", config_.port, strerror(errno));
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }

    if (::listen(listen_fd_, static_cast<int>(config_.max_sessions)) < 0) {
      OSP_LOG_ERROR("TELSH", "listen() failed: %s", strerror(errno));
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }

    running_.store(true, std::memory_order_release);
    accept_thread_ = std::thread([this]() { AcceptLoop(); });

    OSP_LOG_INFO("TELSH", "Listening on port %u (max %u sessions)", config_.port, config_.max_sessions);
    return true;
  }

  /// Stop the server and join all threads.
  void Stop() {
    if (!running_.load(std::memory_order_acquire)) {
      return;
    }

    OSP_LOG_INFO("TELSH", "Stopping server...");
    running_.store(false, std::memory_order_release);

    // Close listen socket to unblock accept()
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }

    // Stop all active sessions
    for (uint32_t i = 0; i < kMaxSessions; ++i) {
      if (slots_[i].active.load(std::memory_order_acquire)) {
        slots_[i].session.Stop();
      }
      if (slots_[i].thread.joinable()) {
        slots_[i].thread.join();
      }
      slots_[i].active.store(false, std::memory_order_release);
    }

    OSP_LOG_INFO("TELSH", "Server stopped");
  }

  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  /// Broadcast raw data to all active sessions.
  void Broadcast(const char* data, uint32_t len) {
    if (data == nullptr || len == 0) {
      return;
    }
    for (uint32_t i = 0; i < kMaxSessions; ++i) {
      if (slots_[i].active.load(std::memory_order_acquire)) {
        slots_[i].session.Send(data, len);
      }
    }
  }

  /// Broadcast printf to all active sessions.
  void BroadcastPrintf(const char* fmt, ...) {
    if (fmt == nullptr) {
      return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
      Broadcast(buf, static_cast<uint32_t>(n));
    }
  }

  /// Global printf (broadcasts via the singleton instance).
  static void Printf(const char* fmt, ...) {
    if (g_instance_ == nullptr || fmt == nullptr) {
      return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
      g_instance_->Broadcast(buf, static_cast<uint32_t>(n));
    }
  }

 private:
  struct SessionSlot {
    TelnetSession session;
    std::thread thread;
    std::atomic<bool> active{false};
  };

  // -----------------------------------------------------------------------
  // Accept loop
  // -----------------------------------------------------------------------
  void AcceptLoop() {
    while (running_.load(std::memory_order_acquire)) {
      struct sockaddr_in client_addr;
      socklen_t addr_len = sizeof(client_addr);

      int32_t fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
      if (fd < 0) {
        if (running_.load(std::memory_order_acquire)) {
          OSP_LOG_WARN("TELSH", "accept() failed: %s", strerror(errno));
        }
        break;
      }

      int32_t slot = FindFreeSlot();
      if (slot < 0) {
        const char* msg = "Server full.\r\n";
        ::send(fd, msg, std::strlen(msg), MSG_NOSIGNAL);
        ::close(fd);
        OSP_LOG_WARN("TELSH", "No free slots, rejected connection");
        continue;
      }

      char ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
      OSP_LOG_INFO("TELSH", "Connection from %s:%u -> slot %d", ip, ntohs(client_addr.sin_port), slot);

      // Build session config
      SessionConfig scfg;
      scfg.username = config_.username;
      scfg.password = config_.password;
      scfg.prompt = config_.prompt;
      if (config_.banner != nullptr) {
        scfg.banner = config_.banner;
      }

      uint32_t idx = static_cast<uint32_t>(slot);
      slots_[idx].session.Init(fd, *registry_, scfg);
      slots_[idx].active.store(true, std::memory_order_release);
      slots_[idx].thread = std::thread([this, idx]() { SessionLoop(idx); });
    }
  }

  // -----------------------------------------------------------------------
  // Session loop
  // -----------------------------------------------------------------------
  void SessionLoop(uint32_t idx) {
    OSP_ASSERT(idx < kMaxSessions);
    slots_[idx].session.Run();
    slots_[idx].active.store(false, std::memory_order_release);
    OSP_LOG_INFO("TELSH", "Slot %u session ended", idx);
  }

  // -----------------------------------------------------------------------
  // Find free slot (join stale thread if needed)
  // -----------------------------------------------------------------------
  int32_t FindFreeSlot() {
    for (uint32_t i = 0; i < config_.max_sessions && i < kMaxSessions; ++i) {
      if (!slots_[i].active.load(std::memory_order_acquire)) {
        if (slots_[i].thread.joinable()) {
          slots_[i].thread.join();
        }
        return static_cast<int32_t>(i);
      }
    }
    return -1;
  }

  // -----------------------------------------------------------------------
  // Member data
  // -----------------------------------------------------------------------
  int32_t listen_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  CommandRegistry* registry_;
  ServerConfig config_;
  SessionSlot slots_[kMaxSessions];

  static inline TelnetServer* g_instance_ = nullptr;
};

// ---------------------------------------------------------------------------
// Global convenience function
// ---------------------------------------------------------------------------

/// Printf to all connected telnet sessions.
inline void tel_printf(const char* fmt, ...) {
  if (TelnetServer::Printf == nullptr || fmt == nullptr) {
    return;
  }
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    TelnetServer::Printf("%s", buf);
  }
}

}  // namespace telsh
