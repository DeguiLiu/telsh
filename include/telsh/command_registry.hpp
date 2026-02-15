// Copyright (c) 2024 liudegui. MIT License.
//
// telsh::CommandRegistry -- zero-allocation command registry for embedded
// telnet debug shell.
//
// Design:
//   - Fixed-capacity array (kMaxCommands = 64), zero heap allocation
//   - Unified command signature: int (*)(int argc, char* argv[], void* ctx)
//   - Thread-safe registration (std::mutex)
//   - In-place ShellSplit for argc/argv parsing (handles quotes)
//   - Built-in "help" command
//   - TELSH_CMD macro for static auto-registration

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace telsh {

// ---------------------------------------------------------------------------
// Command function signature
// ---------------------------------------------------------------------------

/// Command callback: returns 0 on success, non-zero on error.
/// @param argc  argument count (argv[0] is the command name)
/// @param argv  argument vector (modified in-place by ShellSplit)
/// @param ctx   user context pointer (set at registration time)
using CmdFn = int (*)(int argc, char* argv[], void* ctx);

/// Output callback used by Execute to send text back to the caller.
using OutputFn = void (*)(const char* str, uint32_t len, void* ctx);

// ---------------------------------------------------------------------------
// CmdEntry
// ---------------------------------------------------------------------------

struct CmdEntry {
  const char* name;  ///< Command name (must point to static storage)
  const char* desc;  ///< Human-readable description (static storage)
  CmdFn fn;          ///< Callback
  void* ctx;         ///< User context
};

// ---------------------------------------------------------------------------
// ShellSplit -- parse command line in-place into argc/argv
// ---------------------------------------------------------------------------

/// Split @p cmdline in-place.  Handles single/double quotes.
/// @return number of arguments, or -1 on overflow.
inline int ShellSplit(char* cmdline, char* argv[], int max_args) {
  if (cmdline == nullptr || argv == nullptr) {
    return -1;
  }

  int argc = 0;
  char* p = cmdline;
  bool in_sq = false;   // inside single quotes
  bool in_dq = false;   // inside double quotes
  bool in_arg = false;

  while (*p != '\0') {
    const bool is_ws = (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');

    if (is_ws && !in_sq && !in_dq) {
      if (in_arg) {
        *p = '\0';
        in_arg = false;
      }
      ++p;
      continue;
    }

    if (*p == '\'' && !in_dq) {
      in_sq = !in_sq;
      // Remove quote by shifting left
      char* dst = p;
      char* src = p + 1;
      while (*src != '\0') { *dst++ = *src++; }
      *dst = '\0';
      if (!in_arg) {
        if (argc >= max_args) { return -1; }
        argv[argc++] = p;
        in_arg = true;
      }
      continue;  // re-examine same position
    }

    if (*p == '"' && !in_sq) {
      in_dq = !in_dq;
      char* dst = p;
      char* src = p + 1;
      while (*src != '\0') { *dst++ = *src++; }
      *dst = '\0';
      if (!in_arg) {
        if (argc >= max_args) { return -1; }
        argv[argc++] = p;
        in_arg = true;
      }
      continue;
    }

    if (!in_arg) {
      if (argc >= max_args) { return -1; }
      argv[argc++] = p;
      in_arg = true;
    }
    ++p;
  }

  return argc;
}

// ---------------------------------------------------------------------------
// CommandRegistry
// ---------------------------------------------------------------------------

class CommandRegistry {
 public:
  static constexpr uint32_t kMaxCommands = 64;
  static constexpr int kMaxArgs = 32;

  CommandRegistry() = default;

  // Non-copyable, non-movable
  CommandRegistry(const CommandRegistry&) = delete;
  CommandRegistry& operator=(const CommandRegistry&) = delete;

  /// Register a command.  @p name and @p desc must be static storage.
  bool Register(const char* name, const char* desc,
                CmdFn fn, void* ctx = nullptr) {
    if (name == nullptr || fn == nullptr) { return false; }

    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ >= kMaxCommands) { return false; }

    // Reject duplicates
    for (uint32_t i = 0; i < count_; ++i) {
      if (std::strcmp(entries_[i].name, name) == 0) { return false; }
    }

    entries_[count_] = {name, desc, fn, ctx};
    ++count_;
    return true;
  }

  /// Execute a command line (modified in-place).
  /// @param output_fn  callback to send output text
  /// @param output_ctx context for output_fn
  /// @return command return code, -1 = not found, -2 = parse error
  int Execute(char* cmdline, OutputFn output_fn, void* output_ctx) {
    if (cmdline == nullptr) { return -2; }

    char* argv[kMaxArgs];
    int argc = ShellSplit(cmdline, argv, kMaxArgs);
    if (argc < 0) { return -2; }
    if (argc == 0) { return 0; }

    // Built-in: help
    if (std::strcmp(argv[0], "help") == 0) {
      PrintHelp(output_fn, output_ctx);
      return 0;
    }

    // Lookup
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < count_; ++i) {
      if (std::strcmp(entries_[i].name, argv[0]) == 0) {
        return entries_[i].fn(argc, argv, entries_[i].ctx);
      }
    }

    // Not found
    if (output_fn != nullptr) {
      char buf[128];
      int n = std::snprintf(buf, sizeof(buf),
                            "Unknown command: %s\r\n", argv[0]);
      if (n > 0) { output_fn(buf, static_cast<uint32_t>(n), output_ctx); }
    }
    return -1;
  }

  /// Find command by name.
  const CmdEntry* FindByName(const char* name) const {
    if (name == nullptr) { return nullptr; }
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < count_; ++i) {
      if (std::strcmp(entries_[i].name, name) == 0) {
        return &entries_[i];
      }
    }
    return nullptr;
  }

  uint32_t Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }

  /// Iterate all entries.
  template <typename Fn>
  void ForEach(Fn&& visitor) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < count_; ++i) {
      visitor(entries_[i]);
    }
  }

  /// Singleton accessor.
  static CommandRegistry& Instance() {
    static CommandRegistry inst;
    return inst;
  }

 private:
  void PrintHelp(OutputFn output_fn, void* output_ctx) {
    if (output_fn == nullptr) { return; }

    const char* hdr = "Available commands:\r\n";
    output_fn(hdr, static_cast<uint32_t>(std::strlen(hdr)), output_ctx);

    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < count_; ++i) {
      char buf[128];
      int n = std::snprintf(buf, sizeof(buf), "  %-16s - %s\r\n",
                            entries_[i].name,
                            entries_[i].desc ? entries_[i].desc : "");
      if (n > 0) { output_fn(buf, static_cast<uint32_t>(n), output_ctx); }
    }
  }

  CmdEntry entries_[kMaxCommands] = {};
  uint32_t count_ = 0;
  mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// Auto-registration helper
// ---------------------------------------------------------------------------

class CmdAutoReg {
 public:
  CmdAutoReg(const char* name, const char* desc, CmdFn fn, void* ctx = nullptr) {
    CommandRegistry::Instance().Register(name, desc, fn, ctx);
  }
};

/// Register a command with the global registry.
/// Usage:
///   TELSH_CMD(reboot, "Reboot the device") {
///     (void)argc; (void)argv; (void)ctx;
///     // ...
///     return 0;
///   }
#define TELSH_CMD(name, desc)                                       \
  static int telsh_cmd_##name(int argc, char* argv[], void* ctx);   \
  static ::telsh::CmdAutoReg telsh_reg_##name(                      \
      #name, desc, telsh_cmd_##name);                               \
  static int telsh_cmd_##name(int argc, char* argv[], void* ctx)

}  // namespace telsh
