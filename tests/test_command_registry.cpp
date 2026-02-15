// Copyright (c) 2024 liudegui. MIT License.
// Tests for telsh::CommandRegistry and ShellSplit.

#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "telsh/command_registry.hpp"

using namespace telsh;

// ============================================================================
// ShellSplit tests
// ============================================================================

TEST_CASE("ShellSplit: empty string", "[command_registry]") {
  char buf[] = "";
  char* argv[8];
  int argc = ShellSplit(buf, argv, 8);
  REQUIRE(argc == 0);
}

TEST_CASE("ShellSplit: single command", "[command_registry]") {
  char buf[] = "help";
  char* argv[8];
  int argc = ShellSplit(buf, argv, 8);
  REQUIRE(argc == 1);
  REQUIRE(std::strcmp(argv[0], "help") == 0);
}

TEST_CASE("ShellSplit: multiple args", "[command_registry]") {
  char buf[] = "add 1 2";
  char* argv[8];
  int argc = ShellSplit(buf, argv, 8);
  REQUIRE(argc == 3);
  REQUIRE(std::strcmp(argv[0], "add") == 0);
  REQUIRE(std::strcmp(argv[1], "1") == 0);
  REQUIRE(std::strcmp(argv[2], "2") == 0);
}

TEST_CASE("ShellSplit: leading/trailing whitespace", "[command_registry]") {
  char buf[] = "  hello  world  ";
  char* argv[8];
  int argc = ShellSplit(buf, argv, 8);
  REQUIRE(argc == 2);
  REQUIRE(std::strcmp(argv[0], "hello") == 0);
  REQUIRE(std::strcmp(argv[1], "world") == 0);
}

TEST_CASE("ShellSplit: tabs and mixed whitespace", "[command_registry]") {
  char buf[] = "a\tb\t c";
  char* argv[8];
  int argc = ShellSplit(buf, argv, 8);
  REQUIRE(argc == 3);
}

TEST_CASE("ShellSplit: double quotes", "[command_registry]") {
  char buf[] = "echo \"hello world\"";
  char* argv[8];
  int argc = ShellSplit(buf, argv, 8);
  REQUIRE(argc == 2);
  REQUIRE(std::strcmp(argv[0], "echo") == 0);
  REQUIRE(std::strcmp(argv[1], "hello world") == 0);
}

TEST_CASE("ShellSplit: single quotes", "[command_registry]") {
  char buf[] = "echo 'hello world'";
  char* argv[8];
  int argc = ShellSplit(buf, argv, 8);
  REQUIRE(argc == 2);
  REQUIRE(std::strcmp(argv[0], "echo") == 0);
  REQUIRE(std::strcmp(argv[1], "hello world") == 0);
}

TEST_CASE("ShellSplit: overflow returns -1", "[command_registry]") {
  char buf[] = "a b c d";
  char* argv[2];
  int argc = ShellSplit(buf, argv, 2);
  REQUIRE(argc == -1);
}

TEST_CASE("ShellSplit: nullptr input", "[command_registry]") {
  char* argv[4];
  REQUIRE(ShellSplit(nullptr, argv, 4) == -1);
  char buf[] = "test";
  REQUIRE(ShellSplit(buf, nullptr, 4) == -1);
}

// ============================================================================
// CommandRegistry tests
// ============================================================================

static int test_cmd_ok(int argc, char* argv[], void* ctx) {
  (void)argc; (void)argv; (void)ctx;
  return 0;
}

static int test_cmd_fail(int argc, char* argv[], void* ctx) {
  (void)argc; (void)argv; (void)ctx;
  return 42;
}

static int test_cmd_ctx(int argc, char* argv[], void* ctx) {
  (void)argc; (void)argv;
  auto* val = static_cast<int*>(ctx);
  (*val)++;
  return 0;
}

TEST_CASE("CommandRegistry: register and find", "[command_registry]") {
  CommandRegistry reg;
  REQUIRE(reg.Register("test", "A test command", test_cmd_ok));
  REQUIRE(reg.Count() == 1);

  const CmdEntry* entry = reg.FindByName("test");
  REQUIRE(entry != nullptr);
  REQUIRE(std::strcmp(entry->name, "test") == 0);
  REQUIRE(std::strcmp(entry->desc, "A test command") == 0);
}

TEST_CASE("CommandRegistry: reject duplicate", "[command_registry]") {
  CommandRegistry reg;
  REQUIRE(reg.Register("dup", "first", test_cmd_ok));
  REQUIRE_FALSE(reg.Register("dup", "second", test_cmd_ok));
  REQUIRE(reg.Count() == 1);
}

TEST_CASE("CommandRegistry: reject nullptr", "[command_registry]") {
  CommandRegistry reg;
  REQUIRE_FALSE(reg.Register(nullptr, "desc", test_cmd_ok));
  REQUIRE_FALSE(reg.Register("name", "desc", nullptr));
}

TEST_CASE("CommandRegistry: find nonexistent", "[command_registry]") {
  CommandRegistry reg;
  REQUIRE(reg.FindByName("nope") == nullptr);
  REQUIRE(reg.FindByName(nullptr) == nullptr);
}

TEST_CASE("CommandRegistry: execute command", "[command_registry]") {
  CommandRegistry reg;
  reg.Register("ok", "returns 0", test_cmd_ok);
  reg.Register("fail", "returns 42", test_cmd_fail);

  // Capture output
  struct OutCtx { char buf[256]; uint32_t len; };
  OutCtx out = {{}, 0};
  auto output_fn = [](const char* str, uint32_t len, void* ctx) {
    auto* o = static_cast<OutCtx*>(ctx);
    if (o->len + len < sizeof(o->buf)) {
      std::memcpy(o->buf + o->len, str, len);
      o->len += len;
    }
  };

  char cmd1[] = "ok";
  REQUIRE(reg.Execute(cmd1, output_fn, &out) == 0);

  char cmd2[] = "fail";
  REQUIRE(reg.Execute(cmd2, output_fn, &out) == 42);
}

TEST_CASE("CommandRegistry: execute unknown command", "[command_registry]") {
  CommandRegistry reg;

  struct OutCtx { char buf[256]; uint32_t len; };
  OutCtx out = {{}, 0};
  auto output_fn = [](const char* str, uint32_t len, void* ctx) {
    auto* o = static_cast<OutCtx*>(ctx);
    if (o->len + len < sizeof(o->buf)) {
      std::memcpy(o->buf + o->len, str, len);
      o->len += len;
    }
  };

  char cmd[] = "nonexistent";
  REQUIRE(reg.Execute(cmd, output_fn, &out) == -1);
  out.buf[out.len] = '\0';
  REQUIRE(std::strstr(out.buf, "Unknown command") != nullptr);
}

TEST_CASE("CommandRegistry: execute with context", "[command_registry]") {
  CommandRegistry reg;
  int counter = 0;
  reg.Register("inc", "increment", test_cmd_ctx, &counter);

  auto noop = [](const char*, uint32_t, void*) {};
  char cmd[] = "inc";
  reg.Execute(cmd, noop, nullptr);
  REQUIRE(counter == 1);

  char cmd2[] = "inc";
  reg.Execute(cmd2, noop, nullptr);
  REQUIRE(counter == 2);
}

TEST_CASE("CommandRegistry: execute empty line", "[command_registry]") {
  CommandRegistry reg;
  auto noop = [](const char*, uint32_t, void*) {};
  char cmd[] = "";
  REQUIRE(reg.Execute(cmd, noop, nullptr) == 0);
}

TEST_CASE("CommandRegistry: execute with args", "[command_registry]") {
  static int captured_argc = 0;
  static char captured_args[4][32] = {};

  auto capture_cmd = [](int argc, char* argv[], void* ctx) -> int {
    (void)ctx;
    captured_argc = argc;
    for (int i = 0; i < argc && i < 4; ++i) {
      std::strncpy(captured_args[i], argv[i], 31);
    }
    return 0;
  };

  CommandRegistry reg;
  reg.Register("cap", "capture args", capture_cmd);

  auto noop = [](const char*, uint32_t, void*) {};
  char cmd[] = "cap foo bar";
  reg.Execute(cmd, noop, nullptr);

  REQUIRE(captured_argc == 3);
  REQUIRE(std::strcmp(captured_args[0], "cap") == 0);
  REQUIRE(std::strcmp(captured_args[1], "foo") == 0);
  REQUIRE(std::strcmp(captured_args[2], "bar") == 0);
}

TEST_CASE("CommandRegistry: help command", "[command_registry]") {
  CommandRegistry reg;
  reg.Register("test1", "First test", test_cmd_ok);
  reg.Register("test2", "Second test", test_cmd_ok);

  struct OutCtx { char buf[1024]; uint32_t len; };
  OutCtx out = {{}, 0};
  auto output_fn = [](const char* str, uint32_t len, void* ctx) {
    auto* o = static_cast<OutCtx*>(ctx);
    if (o->len + len < sizeof(o->buf)) {
      std::memcpy(o->buf + o->len, str, len);
      o->len += len;
    }
  };

  char cmd[] = "help";
  REQUIRE(reg.Execute(cmd, output_fn, &out) == 0);
  out.buf[out.len] = '\0';
  REQUIRE(std::strstr(out.buf, "test1") != nullptr);
  REQUIRE(std::strstr(out.buf, "test2") != nullptr);
  REQUIRE(std::strstr(out.buf, "First test") != nullptr);
}

TEST_CASE("CommandRegistry: ForEach", "[command_registry]") {
  CommandRegistry reg;
  reg.Register("a", "cmd a", test_cmd_ok);
  reg.Register("b", "cmd b", test_cmd_ok);

  uint32_t count = 0;
  reg.ForEach([&count](const CmdEntry&) { ++count; });
  REQUIRE(count == 2);
}
