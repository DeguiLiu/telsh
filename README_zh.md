# telsh

C++17 header-only 嵌入式 Telnet 调试 Shell，零 boost 依赖，零堆分配，适合资源受限环境。

## 项目信息

- 许可证: MIT
- 作者: liudegui
- Gitee: https://gitee.com/liudegui/telsh
- GitHub: https://github.com/DeguiLiu/telsh

## 核心特性

- C++17 header-only，纯 POSIX socket，无 boost 依赖
- 零堆分配，固定容量数组，适合资源受限嵌入式环境
- Per-session IAC 状态机，无全局状态，多 session 安全
- 固定 session 池 + joinable 线程管理，不 detach，不裸 new
- 统一命令签名: `int (*)(int argc, char* argv[], void* ctx)`
- 原地 ShellSplit 解析 argc/argv，支持引号
- TELSH_CMD 宏静态自动注册命令
- 可选用户名/密码认证
- 命令历史支持（上下箭头）
- 内置 help 命令
- 广播 printf 到所有 session
- 28 个 Catch2 测试用例覆盖

## 快速开始

### 基本使用

```cpp
#include "telsh/telnet_server.hpp"

// 使用宏注册命令
TELSH_CMD(hello, "Print greeting") {
    if (argc < 2) {
        telsh::TelnetServer::Printf("Usage: hello <name>\r\n");
        return 1;
    }
    telsh::TelnetServer::Printf("Hello, %s!\r\n", argv[1]);
    return 0;
}

int main() {
    // 配置服务器
    telsh::ServerConfig config;
    config.port = 2500;
    config.username = "admin";
    config.password = "1234";
    config.max_sessions = 4;

    // 启动服务器
    telsh::TelnetServer server(telsh::CommandRegistry::Instance(), config);
    if (!server.Start()) {
        return 1;
    }

    // 主循环
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    server.Stop();
    return 0;
}
```

### 命令注册方式

#### 1. TELSH_CMD 宏（推荐）

```cpp
TELSH_CMD(status, "Show system status") {
    telsh::TelnetServer::Printf("System OK\r\n");
    return 0;
}
```

#### 2. 自由函数注册

```cpp
int my_command(int argc, char* argv[], void* ctx) {
    telsh::TelnetServer::Printf("Command executed\r\n");
    return 0;
}

// 在初始化时注册
telsh::CommandRegistry::Instance().Register("mycmd", "My command", my_command);
```

#### 3. 带上下文注册

```cpp
struct AppContext {
    int counter;
};

int increment(int argc, char* argv[], void* ctx) {
    auto* app = static_cast<AppContext*>(ctx);
    app->counter++;
    telsh::TelnetServer::Printf("Counter: %d\r\n", app->counter);
    return 0;
}

// 注册时传入上下文
AppContext app_ctx{0};
telsh::CommandRegistry::Instance().Register("inc", "Increment counter", increment, &app_ctx);
```

### 连接到服务器

```bash
telnet localhost 2500
```

输入用户名和密码后，可以使用 `help` 命令查看所有可用命令。

## 构建

### 要求

- CMake 3.14+
- C++17 编译器（GCC 7+, Clang 5+）
- POSIX 兼容系统（Linux, macOS）

### 构建步骤

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

### 运行测试

```bash
ctest --output-on-failure
```

### CMake 选项

- `TELSH_BUILD_TESTS`: 构建测试（默认 ON）
- `TELSH_BUILD_EXAMPLES`: 构建示例（默认 ON）

## API 概览

### CommandRegistry

命令注册表，单例模式，最多支持 64 条命令。

```cpp
class CommandRegistry {
public:
    static CommandRegistry& Instance();

    bool Register(const char* name, const char* description,
                  CommandFunc func, void* context = nullptr);

    const CommandEntry* Find(const char* name) const;

    void ForEach(void (*callback)(const CommandEntry&, void*), void* ctx) const;
};
```

### TelnetSession

单个 Telnet 会话管理，处理 IAC 协议、认证、命令历史。

```cpp
class TelnetSession {
public:
    TelnetSession(int client_fd, const ServerConfig& config,
                  const CommandRegistry& registry);

    void Run();
    void Stop();
    void SendBroadcast(const char* msg, size_t len);
};
```

### TelnetServer

Telnet 服务器，管理固定 session 池。

```cpp
class TelnetServer {
public:
    TelnetServer(const CommandRegistry& registry, const ServerConfig& config);

    bool Start();
    void Stop();

    static void Printf(const char* fmt, ...);
    static void Broadcast(const char* msg, size_t len);
};
```

### ServerConfig

服务器配置结构。

```cpp
struct ServerConfig {
    uint16_t port = 2323;
    const char* username = nullptr;
    const char* password = nullptr;
    uint32_t max_sessions = 8;
    uint32_t max_commands = 64;
    uint32_t max_history = 16;
    uint32_t max_line_length = 256;
};
```

## 架构

### 目录结构

```
telsh/
├── include/
│   ├── osp/                          # 复用自 newosp 的工具头文件
│   │   ├── platform.hpp              # 平台检测、OSP_ASSERT
│   │   ├── vocabulary.hpp            # FixedFunction、FixedString、ScopeGuard
│   │   └── log.hpp                   # 日志宏
│   └── telsh/                        # telsh 核心头文件
│       ├── command_registry.hpp      # 命令注册表（最多 64 条）
│       ├── telnet_session.hpp        # 会话管理（IAC/认证/历史）
│       └── telnet_server.hpp         # 服务器（固定 session 池）
├── examples/
│   ├── basic_demo.cpp                # 基本使用示例
│   └── advanced_demo.cpp             # 高级功能示例
├── tests/
│   ├── test_command_registry.cpp     # 命令注册测试
│   ├── test_telnet_session.cpp       # 会话管理测试
│   └── test_telnet_server.cpp        # 服务器测试
├── CMakeLists.txt
├── README.md
└── README_zh.md
```

### 核心组件

#### 1. CommandRegistry（命令注册表）

- 固定容量数组（最多 64 条命令），零堆分配
- 统一命令签名: `int (*)(int argc, char* argv[], void* ctx)`
- 支持 TELSH_CMD 宏静态自动注册
- 线程安全的查找和遍历

#### 2. TelnetSession（会话管理）

- Per-session IAC 状态机，无全局状态
- 支持用户名/密码认证
- 命令历史（上下箭头导航，最多 16 条）
- 原地 ShellSplit 解析 argc/argv
- 固定缓冲区（256 字节行缓冲）

#### 3. TelnetServer（服务器）

- 固定 session 池（最多 8 个并发连接）
- Joinable 线程管理，不 detach
- 广播消息到所有活跃 session
- 优雅关闭，等待所有线程结束

### 内存管理

- 零堆分配设计
- 固定容量数组（命令表、session 池、历史缓冲）
- 栈分配或静态存储
- 适合资源受限嵌入式环境

### 线程模型

- 主线程: accept 循环
- 每个 session 一个工作线程
- 固定线程池，不动态创建/销毁
- Joinable 线程，优雅关闭



## 测试

项目包含 28 个 Catch2 测试用例，覆盖以下场景:

- 命令注册和查找
- IAC 协议处理
- 认证流程
- 命令历史
- ShellSplit 解析
- Session 生命周期
- 服务器启动/停止
- 广播消息

运行测试:

```bash
cd build
ctest --output-on-failure
```

## 许可证

MIT License

Copyright (c) 2025 liudegui

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
