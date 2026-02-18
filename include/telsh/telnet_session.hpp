// Copyright (c) 2024 liudegui. MIT License.
//
// telsh::TelnetSession -- per-connection telnet session handler.
//
// Design:
//   - Per-session IAC state machine (no global state)
//   - Per-session authentication (optional username/password)
//   - Command history ring buffer (fixed capacity, up/down arrow)
//   - Telnet protocol: IAC negotiation, echo suppression, SGA
//   - Arrow key ESC sequence handling
//   - Ctrl+S/Ctrl+Q flow control
//   - Zero heap allocation

#pragma once

#include "osp/log.hpp"
#include "telsh/command_registry.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <atomic>
#include <sys/socket.h>
#include <unistd.h>

namespace telsh {

// ---------------------------------------------------------------------------
// Telnet protocol constants
// ---------------------------------------------------------------------------

namespace tel {
constexpr uint8_t kSE = 240;
constexpr uint8_t kSB = 250;
constexpr uint8_t kWILL = 251;
constexpr uint8_t kWONT = 252;
constexpr uint8_t kDO = 253;
constexpr uint8_t kDONT = 254;
constexpr uint8_t kIAC = 255;

constexpr uint8_t kOptEcho = 1;
constexpr uint8_t kOptSGA = 3;
constexpr uint8_t kOptNAWS = 31;
constexpr uint8_t kOptLFLOW = 33;
}  // namespace tel

// ---------------------------------------------------------------------------
// SessionConfig
// ---------------------------------------------------------------------------

struct SessionConfig {
  const char* username = nullptr;  ///< nullptr = no auth required
  const char* password = nullptr;
  const char* prompt = "telsh> ";
  const char* banner =
      "*===========================================================*\r\n"
      "  telsh v1.0 -- Embedded Debug Shell\r\n"
      "*===========================================================*\r\n";
};

// ---------------------------------------------------------------------------
// TelnetSession
// ---------------------------------------------------------------------------

class TelnetSession {
 public:
  static constexpr uint32_t kMaxCmdLen = 256;
  static constexpr uint32_t kHistorySize = 16;

  TelnetSession() = default;
  ~TelnetSession() { Close(); }

  // Non-copyable
  TelnetSession(const TelnetSession&) = delete;
  TelnetSession& operator=(const TelnetSession&) = delete;

  /// Initialize session.  Called by TelnetServer before Run().
  void Init(int32_t fd, CommandRegistry& registry, const SessionConfig& cfg) {
    sock_fd_ = fd;
    registry_ = &registry;
    config_ = cfg;
    running_.store(true, std::memory_order_release);

    // Reset all state
    cmd_len_ = 0;
    std::memset(cmd_buf_, 0, sizeof(cmd_buf_));
    std::memset(user_buf_, 0, sizeof(user_buf_));
    std::memset(history_, 0, sizeof(history_));
    history_count_ = 0;
    history_write_ = 0;
    history_nav_ = -1;
    output_paused_ = false;
    iac_ = {};
    arrow_ = ArrowPhase::kNone;

    auth_ = (config_.username != nullptr && config_.password != nullptr) ? Auth::kNeedUser : Auth::kAuthorized;
  }

  /// Main session loop (blocking).  Returns when client disconnects or
  /// Stop() is called.
  void Run() {
    if (sock_fd_ < 0 || registry_ == nullptr) {
      return;
    }

    // Telnet negotiations
    SendIac(tel::kDO, tel::kOptSGA);
    SendIac(tel::kDO, tel::kOptNAWS);
    SendIac(tel::kWILL, tel::kOptEcho);
    SendIac(tel::kWILL, tel::kOptSGA);

    // Welcome banner
    if (config_.banner != nullptr) {
      SendStr(config_.banner);
    }

    // Initial prompt
    ShowPrompt();

    // Read loop -- one byte at a time (telnet character mode)
    uint8_t byte = 0;
    while (running_.load(std::memory_order_acquire)) {
      ssize_t n = ::recv(sock_fd_, &byte, 1, 0);
      if (n <= 0) {
        break;
      }

      char c = FilterIac(byte);
      if (c == '\0') {
        continue;
      }

      ProcessChar(c);
    }

    Close();
  }

  /// Signal session to stop (called from another thread).
  void Stop() {
    running_.store(false, std::memory_order_release);
    // Shutdown socket to unblock recv()
    if (sock_fd_ >= 0) {
      ::shutdown(sock_fd_, SHUT_RDWR);
    }
  }

  /// Close the socket.
  void Close() {
    if (sock_fd_ >= 0) {
      ::close(sock_fd_);
      sock_fd_ = -1;
    }
  }

  /// Send raw bytes (used by TelnetServer::Broadcast).
  void Send(const char* data, uint32_t len) {
    if (data == nullptr || len == 0 || sock_fd_ < 0) {
      return;
    }
    if (output_paused_) {
      return;
    }
    ::send(sock_fd_, data, len, MSG_NOSIGNAL);
  }

  /// Send null-terminated string.
  void SendStr(const char* str) {
    if (str != nullptr) {
      Send(str, static_cast<uint32_t>(std::strlen(str)));
    }
  }

  /// Printf to this session.
  void Printf(const char* fmt, ...) {
    if (fmt == nullptr || sock_fd_ < 0) {
      return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0) {
      Send(buf, static_cast<uint32_t>(len));
    }
  }

 private:
  // --- IAC state machine (per-session) ---
  enum class IacPhase : uint8_t { kNormal, kIac, kNego, kSub };

  struct IacState {
    IacPhase phase = IacPhase::kNormal;
    uint8_t prev_byte = 0;
  };

  // --- Authentication ---
  enum class Auth : uint8_t { kNeedUser, kNeedPass, kAuthorized };

  // --- Arrow key ESC sequence ---
  enum class ArrowPhase : uint8_t { kNone, kEsc, kBracket };

  // -----------------------------------------------------------------------
  // IAC filter -- returns printable char or '\0' if consumed
  // -----------------------------------------------------------------------
  char FilterIac(uint8_t byte) {
    switch (iac_.phase) {
      case IacPhase::kNormal:
        if (byte == tel::kIAC) {
          iac_.phase = IacPhase::kIac;
          return '\0';
        }
        return static_cast<char>(byte);

      case IacPhase::kIac:
        if (byte == tel::kIAC) {
          iac_.phase = IacPhase::kNormal;
          return static_cast<char>(byte);  // escaped 0xFF
        }
        if (byte >= tel::kWILL && byte <= tel::kDONT) {
          iac_.phase = IacPhase::kNego;
          return '\0';
        }
        if (byte == tel::kSB) {
          iac_.phase = IacPhase::kSub;
          iac_.prev_byte = 0;
          return '\0';
        }
        iac_.phase = IacPhase::kNormal;
        return '\0';

      case IacPhase::kNego:
        iac_.phase = IacPhase::kNormal;
        return '\0';

      case IacPhase::kSub:
        if (iac_.prev_byte == tel::kIAC && byte == tel::kSE) {
          iac_.phase = IacPhase::kNormal;
        }
        iac_.prev_byte = byte;
        return '\0';

      default:
        iac_.phase = IacPhase::kNormal;
        return '\0';
    }
  }

  // -----------------------------------------------------------------------
  // Send IAC command
  // -----------------------------------------------------------------------
  void SendIac(uint8_t cmd, uint8_t opt) {
    uint8_t buf[3] = {tel::kIAC, cmd, opt};
    Send(reinterpret_cast<const char*>(buf), 3);
  }

  // -----------------------------------------------------------------------
  // Prompt
  // -----------------------------------------------------------------------
  void ShowPrompt() {
    switch (auth_) {
      case Auth::kNeedUser:
        SendStr("username: ");
        break;
      case Auth::kNeedPass:
        SendStr("password: ");
        break;
      case Auth::kAuthorized:
        SendStr(config_.prompt);
        break;
    }
  }

  // -----------------------------------------------------------------------
  // Character processing
  // -----------------------------------------------------------------------
  void ProcessChar(char c) {
    // Arrow key ESC sequence
    if (arrow_ != ArrowPhase::kNone) {
      HandleArrow(c);
      return;
    }
    if (c == 27) {  // ESC
      arrow_ = ArrowPhase::kEsc;
      return;
    }

    // Flow control
    if (c == 19) {
      output_paused_ = true;
      return;
    }  // Ctrl+S
    if (c == 17) {
      output_paused_ = false;
      return;
    }  // Ctrl+Q

    // Backspace / DEL
    if (c == 8 || c == 127) {
      if (cmd_len_ > 0) {
        --cmd_len_;
        cmd_buf_[cmd_len_] = '\0';
        if (auth_ != Auth::kNeedPass) {
          Send("\b \b", 3);
        }
      }
      return;
    }

    // Enter
    if (c == '\r') {
      Send("\r\n", 2);
      cmd_buf_[cmd_len_] = '\0';

      if (auth_ != Auth::kAuthorized) {
        CheckAuth();
      } else if (cmd_len_ > 0) {
        ExecuteCommand();
      }

      cmd_len_ = 0;
      std::memset(cmd_buf_, 0, kMaxCmdLen);
      history_nav_ = -1;
      ShowPrompt();
      return;
    }

    // Ignore bare LF after CR
    if (c == '\n') {
      return;
    }

    // Printable character
    if (cmd_len_ < kMaxCmdLen - 1) {
      cmd_buf_[cmd_len_++] = c;
      cmd_buf_[cmd_len_] = '\0';
      // Echo (mask password)
      if (auth_ == Auth::kNeedPass) {
        Send("*", 1);
      } else {
        Send(&c, 1);
      }
    }
  }

  // -----------------------------------------------------------------------
  // Arrow key handling
  // -----------------------------------------------------------------------
  void HandleArrow(char c) {
    if (arrow_ == ArrowPhase::kEsc) {
      arrow_ = (c == '[') ? ArrowPhase::kBracket : ArrowPhase::kNone;
      return;
    }

    // ArrowPhase::kBracket
    arrow_ = ArrowPhase::kNone;

    if (c == 'A') {  // Up
      int32_t next = history_nav_ + 1;
      if (next < static_cast<int32_t>(history_count_)) {
        history_nav_ = next;
        ReplaceLineWith(GetHistory(history_nav_));
      }
    } else if (c == 'B') {  // Down
      if (history_nav_ > 0) {
        --history_nav_;
        ReplaceLineWith(GetHistory(history_nav_));
      } else if (history_nav_ == 0) {
        history_nav_ = -1;
        ReplaceLineWith("");
      }
    }
    // C (right) / D (left) ignored for simplicity
  }

  void ReplaceLineWith(const char* text) {
    // Erase current line
    while (cmd_len_ > 0) {
      --cmd_len_;
      Send("\b \b", 3);
    }
    if (text == nullptr) {
      return;
    }
    uint32_t len = static_cast<uint32_t>(std::strlen(text));
    if (len >= kMaxCmdLen) {
      len = kMaxCmdLen - 1;
    }
    std::memcpy(cmd_buf_, text, len);
    cmd_buf_[len] = '\0';
    cmd_len_ = len;
    Send(cmd_buf_, cmd_len_);
  }

  // -----------------------------------------------------------------------
  // Authentication
  // -----------------------------------------------------------------------
  void CheckAuth() {
    if (auth_ == Auth::kNeedUser) {
      std::strncpy(user_buf_, cmd_buf_, sizeof(user_buf_) - 1);
      user_buf_[sizeof(user_buf_) - 1] = '\0';
      auth_ = Auth::kNeedPass;
    } else if (auth_ == Auth::kNeedPass) {
      if (std::strcmp(user_buf_, config_.username) == 0 && std::strcmp(cmd_buf_, config_.password) == 0) {
        auth_ = Auth::kAuthorized;
        SendStr("Login OK.\r\n");
      } else {
        SendStr("Login failed.\r\n");
        auth_ = Auth::kNeedUser;
        std::memset(user_buf_, 0, sizeof(user_buf_));
      }
    }
  }

  // -----------------------------------------------------------------------
  // Command execution
  // -----------------------------------------------------------------------

  /// OutputFn adapter: sends text to this session's socket.
  static void SessionOutput(const char* str, uint32_t len, void* ctx) {
    auto* self = static_cast<TelnetSession*>(ctx);
    if (self != nullptr) {
      self->Send(str, len);
    }
  }

  void ExecuteCommand() {
    PushHistory(cmd_buf_);

    // Built-in: exit
    if (std::strcmp(cmd_buf_, "exit") == 0 || std::strcmp(cmd_buf_, "quit") == 0) {
      SendStr("Bye.\r\n");
      Stop();
      return;
    }

    // Copy cmd_buf_ because Execute modifies it in-place
    char exec_buf[kMaxCmdLen];
    std::strncpy(exec_buf, cmd_buf_, kMaxCmdLen - 1);
    exec_buf[kMaxCmdLen - 1] = '\0';

    registry_->Execute(exec_buf, SessionOutput, this);
  }

  // -----------------------------------------------------------------------
  // Command history (ring buffer)
  // -----------------------------------------------------------------------
  void PushHistory(const char* cmd) {
    if (cmd == nullptr || cmd[0] == '\0') {
      return;
    }

    // Skip duplicate of most recent
    if (history_count_ > 0) {
      uint32_t prev = (history_write_ + kHistorySize - 1) % kHistorySize;
      if (std::strcmp(history_[prev], cmd) == 0) {
        return;
      }
    }

    std::strncpy(history_[history_write_], cmd, kMaxCmdLen - 1);
    history_[history_write_][kMaxCmdLen - 1] = '\0';
    history_write_ = (history_write_ + 1) % kHistorySize;
    if (history_count_ < kHistorySize) {
      ++history_count_;
    }
  }

  /// Get history entry.  0 = most recent.
  const char* GetHistory(int32_t index) const {
    if (index < 0 || index >= static_cast<int32_t>(history_count_)) {
      return nullptr;
    }
    uint32_t pos = (history_write_ + kHistorySize - 1 - static_cast<uint32_t>(index)) % kHistorySize;
    return history_[pos];
  }

  // -----------------------------------------------------------------------
  // Member data
  // -----------------------------------------------------------------------
  int32_t sock_fd_ = -1;
  std::atomic<bool> running_{false};
  CommandRegistry* registry_ = nullptr;
  SessionConfig config_;

  // IAC
  IacState iac_;

  // Auth
  Auth auth_ = Auth::kAuthorized;
  char user_buf_[64] = {};

  // Arrow
  ArrowPhase arrow_ = ArrowPhase::kNone;

  // Command buffer
  char cmd_buf_[kMaxCmdLen] = {};
  uint32_t cmd_len_ = 0;

  // History
  char history_[kHistorySize][kMaxCmdLen] = {};
  uint32_t history_count_ = 0;
  uint32_t history_write_ = 0;
  int32_t history_nav_ = -1;

  // Flow control
  bool output_paused_ = false;
};

}  // namespace telsh
