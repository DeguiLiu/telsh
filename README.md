# telsh - Embedded Telnet Debug Shell

[![CI](https://github.com/DeguiLiu/telsh/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/telsh/actions/workflows/ci.yml)
[![Code Coverage](https://github.com/DeguiLiu/telsh/actions/workflows/coverage.yml/badge.svg)](https://github.com/DeguiLiu/telsh/actions/workflows/coverage.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A C++17 header-only embedded telnet debug shell for industrial embedded systems.

**License:** MIT
**Author:** liudegui
**Repositories:**
- Gitee: https://gitee.com/liudegui/telsh
- GitHub: https://github.com/DeguiLiu/telsh

## Overview

telsh provides a lightweight, zero-dependency telnet server for embedded debugging and diagnostics. Designed for resource-constrained environments, it uses fixed-capacity data structures and avoids heap allocation in hot paths.

## Features

- **Header-only:** Single-include integration, no separate compilation
- **Zero external dependencies:** Pure POSIX sockets, no Boost or third-party libraries
- **Zero heap allocation:** Fixed-capacity arrays for commands, sessions, and buffers
- **Multi-session safe:** Per-session IAC state machine, no global state
- **Thread-safe:** Fixed session pool with joinable threads (no detach, no naked new)
- **Unified command interface:** `int (*)(int argc, char* argv[], void* ctx)`
- **Static auto-registration:** `TELSH_CMD` macro for compile-time command registration
- **Authentication:** Optional username/password login
- **Command history:** Up/down arrow key navigation
- **Built-in help:** Auto-generated command list
- **Broadcast support:** `Printf` to all active sessions
- **Fully tested:** 28 Catch2 test cases, all passing

## Quick Start

### Basic Server

```cpp
#include "telsh/telnet_server.hpp"

// Define a command
TELSH_CMD(hello, "Print greeting") {
    telsh::TelnetServer::Printf("Hello, %s!\r\n", argc > 1 ? argv[1] : "World");
    return 0;
}

int main() {
    // Configure server
    telsh::ServerConfig config;
    config.port = 2500;
    config.username = "admin";
    config.password = "1234";

    // Start server
    telsh::TelnetServer server(telsh::CommandRegistry::Instance(), config);
    server.Start();

    // Main loop
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    server.Stop();
    return 0;
}
```

### Connect

```bash
telnet 127.0.0.1 2500
```

### Example Session

```
Trying 127.0.0.1...
Connected to 127.0.0.1.
Escape character is '^]'.
Username: admin
Password:
Welcome to telsh debug shell
Type 'help' for available commands
> help
Available commands:
  hello - Print greeting
  help  - Show available commands
> hello Alice
Hello, Alice!
> exit
Connection closed by foreign host.
```

## Build

### Requirements

- CMake 3.14+
- C++17 compiler (GCC 7+, Clang 5+)
- POSIX-compliant OS (Linux, macOS)

### Build Steps

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Run Tests

```bash
ctest --output-on-failure
```

### CMake Options

- `TELSH_BUILD_TESTS` - Build test suite (default: ON)
- `TELSH_BUILD_EXAMPLES` - Build example programs (default: ON)

## API

### Command Registration

```cpp
// Static registration (preferred)
TELSH_CMD(cmd_name, "Description") {
    // argc: argument count
    // argv: argument array (argv[0] is command name)
    // ctx: user context pointer
    return 0; // 0 = success, non-zero = error
}

// Dynamic registration
telsh::CommandRegistry::Instance().Register("cmd_name", handler, "Description");
```

### Server Configuration

```cpp
telsh::ServerConfig config;
config.port = 2500;                    // Listen port
config.max_sessions = 4;               // Max concurrent sessions (default: 8)
config.username = "admin";             // Optional authentication
config.password = "1234";
config.prompt = "> ";                  // Command prompt
config.welcome_msg = "Welcome!\r\n";   // Login banner
```

### Server Control

```cpp
telsh::TelnetServer server(registry, config);
server.Start();                        // Start listening
server.Stop();                         // Stop and close all sessions
server.Printf("msg\r\n");              // Broadcast to all sessions
```

### Command Parsing

Commands are parsed using `ShellSplit`, which handles:
- Whitespace separation
- Single and double quotes
- Escape sequences (`\"`, `\\`)

Example: `cmd "arg with spaces" 'another arg'` → `argc=3`

## Architecture

### Header Files

**Core (3 files):**
- `include/telsh/command_registry.hpp` - Command registration and lookup (64 commands max)
- `include/telsh/telnet_session.hpp` - Session management (IAC state machine, auth, history)
- `include/telsh/telnet_server.hpp` - Server (fixed session pool, max 8 concurrent)

**Utilities (3 files from newosp):**
- `include/osp/platform.hpp` - Platform detection, `OSP_ASSERT`
- `include/osp/vocabulary.hpp` - `FixedFunction`, `FixedString`, `ScopeGuard`
- `include/osp/log.hpp` - Logging macros

### Design Principles

- **Fixed capacity:** All containers use compile-time size limits
- **No heap allocation:** Stack-based buffers and fixed arrays
- **Thread-per-session:** Each session runs in a dedicated thread (joinable, not detached)
- **IAC state machine:** Per-session telnet protocol handling (no global state)
- **RAII:** `ScopeGuard` for resource cleanup, no naked pointers

### Limits

- Max commands: 64 (configurable via `kMaxCommands`)
- Max sessions: 8 (configurable via `ServerConfig::max_sessions`)
- Max command length: 256 bytes
- Max history entries: 16 per session
- Max arguments: 32 per command

## Examples

See `examples/` directory:
- `basic_demo.cpp` - Minimal server with custom commands
- `auth_demo.cpp` - Server with authentication
- `broadcast_demo.cpp` - Broadcasting messages to all sessions

## Testing

28 test cases covering:
- Command registration and lookup
- Argument parsing (quotes, escapes)
- IAC state machine (WILL/WONT/DO/DONT)
- Session lifecycle (connect, auth, disconnect)
- Multi-session concurrency
- History navigation

Run tests:
```bash
cd build
ctest --output-on-failure
```

## License

MIT License. See LICENSE file for details.

## Contributing

Contributions welcome. Please ensure:
- Code passes all tests
- Follows existing style (Google C++ Style Guide)
- No external dependencies added
- No heap allocation in hot paths

## Author

liudegui (dgliu)
