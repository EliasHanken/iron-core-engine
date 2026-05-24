# Typed Message Dispatcher + Tag Game (M8.3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `iron::MessageRegistry` (templated typed-message layer over `NetTransport`) + a shared `iron::parseNetArgs` CLI helper, refactor `games/05-net-cubes` to use both (deleting its hand-rolled protocol), and ship a brand-new `games/06-net-tag` (host-authoritative N-player tag with scoring, LAN cross-machine play).

**Architecture:** `MessageRegistry` wraps a `NetTransport*` and installs its own `onMessage` handler at construction. Game code declares POD messages with `static constexpr std::uint8_t kTag`, calls `registry.send<Msg>(...)` and `registry.registerHandler<Msg>(...)`. Wire format `[u8 tag][raw memcpy of struct]`. Tag namespace is per-registry. `parseNetArgs(argc, argv)` returns `{ NetAddress, wantsConnect }`; reused by both demos.

**Tech Stack:** C++23 templates, `std::is_trivially_copyable_v`, `std::span<const std::byte>`, `std::function`, MockTransport for tests.

**Spec:** `docs/superpowers/specs/2026-05-24-typed-dispatcher-and-tag-design.md`

---

## File Structure

**New files:**
- `engine/core/NetArgs.h` + `.cpp` — `parseNetArgs(argc, argv)` returning `{addr, wantsConnect}`
- `engine/net/MessageRegistry.h` — templated public API (header-only templates) + `MessageRegistryImpl` non-template helpers
- `engine/net/MessageRegistry.cpp` — non-template implementation pieces (ctor wires up transport->setOnMessage, dispatch helpers)
- `tests/test_net_args.cpp` — parseNetArgs unit tests
- `tests/test_message_registry.cpp` — round-trip + edge cases via MockTransport
- `games/05-net-cubes/Messages.h` — kTag-style POD message structs (replaces Protocol.h)
- `games/06-net-tag/CMakeLists.txt`
- `games/06-net-tag/Messages.h` — 6 typed messages
- `games/06-net-tag/main.cpp` — host-authoritative tag game

**Modified files:**
- `engine/CMakeLists.txt` — add `core/NetArgs.cpp` + `net/MessageRegistry.cpp` to ironcore
- `tests/CMakeLists.txt` — add `test_net_args` + `test_message_registry`; remove `test_net_cubes_protocol`
- `games/05-net-cubes/CMakeLists.txt` — drop Protocol.cpp from sources
- `games/05-net-cubes/main.cpp` — use MessageRegistry + parseNetArgs
- `CMakeLists.txt` (top-level) — `add_subdirectory(games/06-net-tag)`
- `docs/engine/networking.md` — add typed-dispatch + tag-game sections

**Deleted files:**
- `games/05-net-cubes/Protocol.h`
- `games/05-net-cubes/Protocol.cpp`
- `tests/test_net_cubes_protocol.cpp`

---

## Task 0: Branch setup

**Files:** none (git state only)

`main` is protected — work goes on a feature branch.

- [ ] **Step 1: Create and switch to the feature branch**

```powershell
git checkout -b feat/typed-dispatcher-and-tag
git status
```

Expected: `On branch feat/typed-dispatcher-and-tag`. Modified docs files (CRLF/LF noise) are harmless.

---

## Task 1: `iron::parseNetArgs` CLI helper (TDD)

**Files:**
- Create: `engine/core/NetArgs.h`
- Create: `engine/core/NetArgs.cpp`
- Create: `tests/test_net_args.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

A tiny helper for both `net-cubes` and `net-tag`. Pure function, no GNS dependency, ideal TDD target.

- [ ] **Step 1: Write the failing test**

Create `tests/test_net_args.cpp`:

```cpp
#include "test_framework.h"
#include "core/NetArgs.h"

#include <cstring>

using namespace iron;

namespace {

// Helper: build a char* argv from string literals; returns argc + argv.
struct Argv {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;
    int argc() const { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

Argv makeArgv(std::initializer_list<const char*> args) {
    Argv a;
    for (const char* s : args) a.storage.emplace_back(s);
    a.ptrs.reserve(a.storage.size());
    for (auto& s : a.storage) a.ptrs.push_back(s.data());
    return a;
}

}  // namespace

int main() {
    // No args → default listen on 127.0.0.1:30005
    {
        Argv a = makeArgv({"net-tag.exe"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == false);
        CHECK(out.addr.ipv4 == 0x7F000001u);
        CHECK(out.addr.port == 30005);
    }

    // --connect <ip> → wantsConnect, parsed IP, default port
    {
        Argv a = makeArgv({"net-tag.exe", "--connect", "192.168.1.5"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == true);
        CHECK(out.addr.ipv4 == ((192u << 24) | (168u << 16) | (1u << 8) | 5u));
        CHECK(out.addr.port == 30005);
    }

    // --port <n> → default IP, parsed port
    {
        Argv a = makeArgv({"net-tag.exe", "--port", "40000"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == false);
        CHECK(out.addr.ipv4 == 0x7F000001u);
        CHECK(out.addr.port == 40000);
    }

    // Both flags
    {
        Argv a = makeArgv({"net-tag.exe", "--connect", "10.0.0.1", "--port", "40000"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == true);
        CHECK(out.addr.ipv4 == ((10u << 24) | (0u << 16) | (0u << 8) | 1u));
        CHECK(out.addr.port == 40000);
    }

    // --connect missing value (last arg) → flag ignored, defaults preserved
    {
        Argv a = makeArgv({"net-tag.exe", "--connect"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == false);
        CHECK(out.addr.ipv4 == 0x7F000001u);
    }

    // --port malformed → port stays default
    {
        Argv a = makeArgv({"net-tag.exe", "--port", "abc"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.addr.port == 30005);
    }

    // --connect with malformed IP → wantsConnect stays false, IP stays default
    {
        Argv a = makeArgv({"net-tag.exe", "--connect", "not.an.ip"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == false);
        CHECK(out.addr.ipv4 == 0x7F000001u);
    }

    // Unknown args silently ignored
    {
        Argv a = makeArgv({"net-tag.exe", "--unknown", "--port", "42"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.addr.port == 42);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Verify the test fails to compile**

```powershell
cmake --build build --target test_net_args
```

Expected: `'core/NetArgs.h': No such file or directory`.

- [ ] **Step 3: Create `engine/core/NetArgs.h`**

```cpp
#pragma once

#include "net/NetTransport.h"

namespace iron {

// Result of parsing networking CLI flags.
//   wantsConnect == false → caller should call transport.listen(addr)
//   wantsConnect == true  → caller should call transport.connect(addr)
//
// `addr.ipv4` is host-byte-order (so 127.0.0.1 == 0x7F000001).
struct NetArgs {
    NetAddress addr{0x7F000001u, 30005};
    bool wantsConnect = false;
};

// Parses `--connect <ip>` and `--port <n>` from argv. Unknown args are
// silently ignored. Malformed values (e.g. `--port abc`) leave the
// corresponding field at its default with a Log::warn for visibility.
// Returns sensible defaults if neither flag is present.
NetArgs parseNetArgs(int argc, char** argv);

} // namespace iron
```

- [ ] **Step 4: Create `engine/core/NetArgs.cpp`**

```cpp
#include "core/NetArgs.h"

#include "core/Log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace iron {

namespace {

// Parse "a.b.c.d" into a host-byte-order uint32_t. Returns false if any
// octet is missing, non-numeric, or out of [0, 255].
bool parseIPv4(std::string_view s, std::uint32_t& out) {
    std::uint32_t octets[4] = {0, 0, 0, 0};
    int idx = 0;
    std::size_t i = 0;
    while (i < s.size() && idx < 4) {
        std::uint32_t val = 0;
        bool any = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            val = val * 10 + static_cast<std::uint32_t>(s[i] - '0');
            if (val > 255) return false;
            ++i;
            any = true;
        }
        if (!any) return false;
        octets[idx++] = val;
        if (idx == 4) break;
        if (i >= s.size() || s[i] != '.') return false;
        ++i;
    }
    if (idx != 4 || i != s.size()) return false;
    out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return true;
}

}  // namespace

NetArgs parseNetArgs(int argc, char** argv) {
    NetArgs out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--connect" && i + 1 < argc) {
            std::uint32_t ip = 0;
            if (parseIPv4(argv[i + 1], ip)) {
                out.addr.ipv4 = ip;
                out.wantsConnect = true;
            } else {
                Log::warn("parseNetArgs: --connect value '%s' is not a valid IPv4",
                          argv[i + 1]);
            }
            ++i;
        } else if (arg == "--port" && i + 1 < argc) {
            char* end = nullptr;
            const long val = std::strtol(argv[i + 1], &end, 10);
            if (end != argv[i + 1] && *end == '\0' && val > 0 && val < 65536) {
                out.addr.port = static_cast<std::uint16_t>(val);
            } else {
                Log::warn("parseNetArgs: --port value '%s' is not a valid port",
                          argv[i + 1]);
            }
            ++i;
        }
        // Unknown flags silently ignored.
    }
    return out;
}

} // namespace iron
```

- [ ] **Step 5: Wire CMake**

Edit `engine/CMakeLists.txt`. Add `core/NetArgs.cpp` to the `ironcore` source list (insert near other `core/` entries, e.g. after `core/Application.cpp`):

```cmake
  core/Application.cpp
  core/NetArgs.cpp
```

Edit `tests/CMakeLists.txt`. Add this line near the other `iron_add_test(...)` calls (e.g. after the `test_mock_net_transport` line):

```cmake
iron_add_test(test_net_args test_net_args.cpp)
```

- [ ] **Step 6: Build + run the test**

```powershell
cmake --build build --target test_net_args
ctest --test-dir build -C Debug -R test_net_args --output-on-failure
```

Use `timeout: 120000`. Expected: `OK - all checks passed`, CTest PASS.

- [ ] **Step 7: Commit**

```powershell
git add engine/core/NetArgs.h engine/core/NetArgs.cpp tests/test_net_args.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "Engine: parseNetArgs(argc, argv) helper for --connect/--port" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: `iron::MessageRegistry` (TDD)

**Files:**
- Create: `engine/net/MessageRegistry.h`
- Create: `engine/net/MessageRegistry.cpp`
- Create: `tests/test_message_registry.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

Templated typed-message layer. Header carries the template bits; `.cpp` carries the non-template ctor/dispatch.

- [ ] **Step 1: Write the failing test**

Create `tests/test_message_registry.cpp`:

```cpp
#include "test_framework.h"
#include "net/MessageRegistry.h"
#include "net/NetTransport.h"
#include "net/backends/mock/MockTransport.h"

#include <cstring>
#include <string>
#include <vector>

using namespace iron;

namespace {

// Test message types. POD, kTag, distinct sizes for the wrong-size test.
struct Foo { static constexpr std::uint8_t kTag = 1; std::uint32_t value; };
struct Bar { static constexpr std::uint8_t kTag = 2; float a; float b; };
struct Baz { static constexpr std::uint8_t kTag = 3; std::uint8_t flag; };

}  // namespace

int main() {
    constexpr NetAddress kAddr{0x7F000001u, 5555};

    // Round-trip per type.
    {
        MockTransport srv, cli;
        MessageRegistry srvReg(&srv);
        MessageRegistry cliReg(&cli);

        Foo gotFoo{};
        Bar gotBar{};
        Baz gotBaz{};
        int fooCount = 0, barCount = 0, bazCount = 0;

        srvReg.registerHandler<Foo>([&](ConnectionId, const Foo& m) {
            gotFoo = m; ++fooCount;
        });
        srvReg.registerHandler<Bar>([&](ConnectionId, const Bar& m) {
            gotBar = m; ++barCount;
        });
        srvReg.registerHandler<Baz>([&](ConnectionId, const Baz& m) {
            gotBaz = m; ++bazCount;
        });

        CHECK(srv.start()); CHECK(srv.listen(kAddr));
        CHECK(cli.start());
        const ConnectionId c = cli.connect(kAddr);
        CHECK(c != kInvalidConnection);
        srv.poll(); cli.poll();  // handshake

        CHECK(cliReg.send<Foo>(c, Foo{42}, SendReliability::Reliable));
        CHECK(cliReg.send<Bar>(c, Bar{1.5f, 2.5f}, SendReliability::Reliable));
        CHECK(cliReg.send<Baz>(c, Baz{7}, SendReliability::Reliable));
        srv.poll();

        CHECK(fooCount == 1); CHECK(gotFoo.value == 42);
        CHECK(barCount == 1); CHECK_NEAR(gotBar.a, 1.5f); CHECK_NEAR(gotBar.b, 2.5f);
        CHECK(bazCount == 1); CHECK(gotBaz.flag == 7);
    }

    // Wrong-size payload is silently dropped (no crash, no handler call).
    {
        MockTransport srv, cli;
        MessageRegistry srvReg(&srv);

        int fooCount = 0;
        srvReg.registerHandler<Foo>([&](ConnectionId, const Foo&) { ++fooCount; });

        CHECK(srv.start()); CHECK(srv.listen(kAddr));
        CHECK(cli.start());
        const ConnectionId c = cli.connect(kAddr);
        srv.poll(); cli.poll();

        // Send raw bytes [tag=Foo's tag][only 2 bytes of payload] via the
        // underlying transport, bypassing the registry's send<>.
        std::byte raw[3];
        raw[0] = std::byte{Foo::kTag};
        raw[1] = std::byte{0};
        raw[2] = std::byte{0};
        CHECK(cli.send(c, std::span<const std::byte>(raw, 3),
                       SendReliability::Reliable));
        srv.poll();

        CHECK(fooCount == 0);
    }

    // Unregistered tag silently dropped.
    {
        MockTransport srv, cli;
        MessageRegistry srvReg(&srv);

        // Only register Foo, never Bar.
        srvReg.registerHandler<Foo>([&](ConnectionId, const Foo&) {});

        CHECK(srv.start()); CHECK(srv.listen(kAddr));
        CHECK(cli.start());
        const ConnectionId c = cli.connect(kAddr);
        srv.poll(); cli.poll();

        // Build a Bar payload and send it via the raw transport with Bar's tag.
        const Bar payload{1.0f, 2.0f};
        std::byte raw[1 + sizeof(Bar)];
        raw[0] = std::byte{Bar::kTag};
        std::memcpy(&raw[1], &payload, sizeof(Bar));
        CHECK(cli.send(c, std::span<const std::byte>(raw, sizeof(raw)),
                       SendReliability::Reliable));
        srv.poll();
        // No crash — Bar handler not registered, message dropped.
    }

    // Multiple messages in one poll dispatched in order.
    {
        MockTransport srv, cli;
        MessageRegistry srvReg(&srv);
        MessageRegistry cliReg(&cli);

        std::vector<std::uint32_t> received;
        srvReg.registerHandler<Foo>([&](ConnectionId, const Foo& m) {
            received.push_back(m.value);
        });

        CHECK(srv.start()); CHECK(srv.listen(kAddr));
        CHECK(cli.start());
        const ConnectionId c = cli.connect(kAddr);
        srv.poll(); cli.poll();

        cliReg.send<Foo>(c, Foo{10}, SendReliability::Reliable);
        cliReg.send<Foo>(c, Foo{20}, SendReliability::Reliable);
        cliReg.send<Foo>(c, Foo{30}, SendReliability::Reliable);
        srv.poll();

        CHECK(received.size() == 3);
        CHECK(received[0] == 10);
        CHECK(received[1] == 20);
        CHECK(received[2] == 30);
    }

    // send<Msg> on kInvalidConnection returns false.
    {
        MockTransport t;
        MessageRegistry reg(&t);
        CHECK(t.start());
        CHECK(!reg.send<Foo>(kInvalidConnection, Foo{1}, SendReliability::Reliable));
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Verify the test fails to compile**

```powershell
cmake --build build --target test_message_registry
```

Expected: header not found.

- [ ] **Step 3: Create `engine/net/MessageRegistry.h`**

```cpp
#pragma once

#include "core/Log.h"
#include "net/NetTransport.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <type_traits>
#include <unordered_map>

namespace iron {

// Typed message dispatch on top of NetTransport. Wraps a single
// transport instance; installs its own onMessage handler at
// construction. Game code must NOT call transport->setOnMessage()
// after wrapping it in a MessageRegistry.
//
// Wire format: [u8 tag][raw memcpy of msg]
//
// Message type requirements (enforced by static_assert):
//   - std::is_trivially_copyable_v<Msg>
//   - Msg::kTag is a `static constexpr std::uint8_t`
//   - 1 + sizeof(Msg) <= kMaxPayloadBytes
//
// Tag 0 is reserved (treated as "invalid"). Tag namespace is per
// registry; different games can reuse tag values without conflict.
class MessageRegistry {
public:
    // Conservative MTU-ish ceiling so a single message can't fragment
    // unreliable delivery on a normal LAN.
    static constexpr std::size_t kMaxPayloadBytes = 1200;

    explicit MessageRegistry(NetTransport* transport);
    ~MessageRegistry();

    // Non-copyable, non-movable: holds a transport pointer and the
    // transport holds our dispatch lambda — copying would alias state.
    MessageRegistry(const MessageRegistry&) = delete;
    MessageRegistry& operator=(const MessageRegistry&) = delete;

    template <typename Msg>
    void registerHandler(std::function<void(ConnectionId, const Msg&)> fn) {
        static_assert(std::is_trivially_copyable_v<Msg>,
                      "MessageRegistry: Msg must be trivially copyable (POD)");
        static_assert(sizeof(typename std::remove_cv_t<decltype(Msg::kTag)>) == 1,
                      "MessageRegistry: Msg::kTag must be a uint8_t");
        static_assert(Msg::kTag != 0,
                      "MessageRegistry: tag 0 is reserved");
        static_assert(1 + sizeof(Msg) <= kMaxPayloadBytes,
                      "MessageRegistry: message too large");

        handlers_[Msg::kTag] = [fn = std::move(fn)](
                ConnectionId conn, std::span<const std::byte> payload) {
            if (payload.size() != sizeof(Msg)) {
                Log::warn("MessageRegistry: dropped Msg tag=%u, payload size %zu "
                          "!= sizeof(Msg) %zu",
                          static_cast<unsigned>(Msg::kTag),
                          payload.size(), sizeof(Msg));
                return;
            }
            Msg msg;
            std::memcpy(&msg, payload.data(), sizeof(Msg));
            fn(conn, msg);
        };
    }

    template <typename Msg>
    bool send(ConnectionId conn, const Msg& msg, SendReliability reliability) {
        static_assert(std::is_trivially_copyable_v<Msg>,
                      "MessageRegistry: Msg must be trivially copyable (POD)");
        static_assert(sizeof(typename std::remove_cv_t<decltype(Msg::kTag)>) == 1,
                      "MessageRegistry: Msg::kTag must be a uint8_t");
        static_assert(Msg::kTag != 0,
                      "MessageRegistry: tag 0 is reserved");
        static_assert(1 + sizeof(Msg) <= kMaxPayloadBytes,
                      "MessageRegistry: message too large");

        if (conn == kInvalidConnection) return false;
        std::byte buf[1 + sizeof(Msg)];
        buf[0] = std::byte{Msg::kTag};
        std::memcpy(&buf[1], &msg, sizeof(Msg));
        return transport_->send(
            conn, std::span<const std::byte>(buf, sizeof(buf)), reliability);
    }

    template <typename Msg>
    void sendToAll(std::span<const ConnectionId> conns,
                   const Msg& msg, SendReliability reliability) {
        for (ConnectionId c : conns) {
            send<Msg>(c, msg, reliability);
        }
    }

private:
    // Dispatch entry point — non-template so it can live in the .cpp.
    void dispatch(ConnectionId conn, std::span<const std::byte> bytes);

    NetTransport* transport_;
    std::unordered_map<std::uint8_t,
        std::function<void(ConnectionId, std::span<const std::byte>)>> handlers_;
};

} // namespace iron
```

- [ ] **Step 4: Create `engine/net/MessageRegistry.cpp`**

```cpp
#include "net/MessageRegistry.h"

namespace iron {

MessageRegistry::MessageRegistry(NetTransport* transport)
    : transport_(transport) {
    transport_->setOnMessage(
        [this](ConnectionId conn, std::span<const std::byte> bytes) {
            this->dispatch(conn, bytes);
        });
}

MessageRegistry::~MessageRegistry() {
    // Defensive: drop our dispatch lambda from the transport so it can
    // be reused (or destroyed) without dangling reference to this.
    if (transport_) {
        transport_->setOnMessage(NetTransport::OnMessageFn{});
    }
}

void MessageRegistry::dispatch(ConnectionId conn,
                                std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        Log::warn("MessageRegistry: dropped empty payload");
        return;
    }
    const std::uint8_t tag = static_cast<std::uint8_t>(bytes[0]);
    if (tag == 0) {
        Log::warn("MessageRegistry: dropped message with reserved tag=0");
        return;
    }
    auto it = handlers_.find(tag);
    if (it == handlers_.end()) {
        Log::warn("MessageRegistry: no handler for tag=%u",
                  static_cast<unsigned>(tag));
        return;
    }
    it->second(conn, bytes.subspan(1));
}

} // namespace iron
```

- [ ] **Step 5: Wire CMake**

Edit `engine/CMakeLists.txt`. Add `net/MessageRegistry.cpp` to the `ironcore` source list (next to `net/NetTransport.cpp`):

```cmake
  net/NetTransport.cpp
  net/MessageRegistry.cpp
```

Edit `tests/CMakeLists.txt`. Add this line (e.g. after `test_net_args` from Task 1):

```cmake
iron_add_test(test_message_registry test_message_registry.cpp)
```

- [ ] **Step 6: Build + run the test**

```powershell
cmake --build build --target test_message_registry
ctest --test-dir build -C Debug -R test_message_registry --output-on-failure
```

Use `timeout: 180000`. Expected: `OK - all checks passed`, CTest PASS.

- [ ] **Step 7: Commit**

```powershell
git add engine/net/MessageRegistry.h engine/net/MessageRegistry.cpp tests/test_message_registry.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "Engine: MessageRegistry (typed dispatch over NetTransport)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: Refactor `games/05-net-cubes` onto MessageRegistry + parseNetArgs

**Files:**
- Create: `games/05-net-cubes/Messages.h`
- Modify: `games/05-net-cubes/main.cpp`
- Modify: `games/05-net-cubes/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Delete: `games/05-net-cubes/Protocol.h`
- Delete: `games/05-net-cubes/Protocol.cpp`
- Delete: `tests/test_net_cubes_protocol.cpp`

Drops the hand-rolled protocol entirely; uses the new dispatcher.

- [ ] **Step 1: Create `games/05-net-cubes/Messages.h`**

```cpp
#pragma once

#include <cstdint>

namespace iron::netcubes {

struct HelloMsg {
    static constexpr std::uint8_t kTag = 1;
    std::uint32_t peerId;
};

struct PositionMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t peerId;
    float x, y, z;
};

} // namespace iron::netcubes
```

- [ ] **Step 2: Rewrite `games/05-net-cubes/main.cpp` to use MessageRegistry + parseNetArgs**

The skeleton (window, FreeFlyCamera-replaced-by-chase, scene, lit shader) STAYS as-is. Three blocks change:
- `#include "Protocol.h"` → `#include "Messages.h"`
- `transport.setOn...` blocks → `MessageRegistry reg(&transport);` + `reg.registerHandler<...>(...)` calls
- The broadcast block's `writePosition(...)` + `transport.send(...)` → `reg.send<PositionMsg>(...)`
- The hardcoded `iron::NetAddress addr{0x7F000001, port};` (after NET_CUBES_PORT env-var read) → `iron::parseNetArgs(argc, argv)` to get addr + wantsConnect

Read the current `games/05-net-cubes/main.cpp` first to understand the surrounding structure. The pieces to change:

**Includes** — replace `#include "Protocol.h"` with:
```cpp
#include "Messages.h"
#include "core/NetArgs.h"
#include "net/MessageRegistry.h"
```

**`main()` signature** — change `int main()` to `int main(int argc, char** argv)`.

**Replace the `NET_CUBES_PORT` env-var block** (and the hardcoded address build) with:
```cpp
    const iron::NetArgs netArgs = iron::parseNetArgs(argc, argv);
    const iron::NetAddress addr = netArgs.addr;
    const bool wantsConnect = netArgs.wantsConnect;
```

(You can delete the `#include <cstdlib>` for getenv and the `#define _CRT_SECURE_NO_WARNINGS` line at the top — they're no longer needed.)

**Replace the `transport.setOnMessage([...])` block** with a `MessageRegistry` and two `registerHandler` calls. The current block looks like:
```cpp
transport.setOnMessage([&](iron::ConnectionId c,
                            std::span<const std::byte> bytes) {
    auto parsed = iron::netcubes::parse(bytes);
    if (!parsed) { ... }
    if (parsed->tag == iron::netcubes::MsgTag::Hello) { ... }
    else if (parsed->tag == iron::netcubes::MsgTag::Position) { ... }
});
```

Change to:
```cpp
iron::MessageRegistry registry(&transport);

registry.registerHandler<iron::netcubes::HelloMsg>(
    [&](iron::ConnectionId /*c*/, const iron::netcubes::HelloMsg& msg) {
        if (isHost) {
            iron::Log::warn("net-cubes: host received Hello — ignoring");
            return;
        }
        if (myPeerId == 0) {
            myPeerId = msg.peerId;
        }
    });

registry.registerHandler<iron::netcubes::PositionMsg>(
    [&](iron::ConnectionId c, const iron::netcubes::PositionMsg& msg) {
        // Validate: host requires the sender's peerId to match what we
        // assigned them. Rejects spoofs (incl. accidental peerId=0
        // that would clobber the host's own cube).
        if (isHost) {
            auto it = connToPeerId.find(c);
            if (it == connToPeerId.end() || it->second != msg.peerId) {
                iron::Log::warn("net-cubes: dropping PositionMsg with mismatched peerId");
                return;
            }
        }
        const iron::Vec3 incoming{msg.x, msg.y, msg.z};
        auto [it, inserted] = cubes.try_emplace(msg.peerId);
        if (inserted) {
            it->second.displayed = incoming;
        }
        it->second.target = incoming;
        // Host rebroadcasts to all other clients (star topology).
        if (isHost) {
            for (const auto& [otherConn, _] : connToPeerId) {
                if (otherConn == c) continue;
                registry.send<iron::netcubes::PositionMsg>(
                    otherConn, msg, iron::SendReliability::Unreliable);
            }
        }
    });
```

**MessageRegistry MUST be constructed AFTER the transport but BEFORE transport.start()**, because its ctor sets the transport's onMessage handler — and our previous code calls `transport.setOn...` setters in a specific order. The registry replaces the onMessage path entirely; onConnectionOpened and onConnectionClosed still go through `transport.setOnConnectionOpened`/`setOnConnectionClosed` directly.

**Replace the `transport.setOnConnectionOpened` host-side Hello send** — find this block:
```cpp
transport.setOnConnectionOpened([&](iron::ConnectionId c) {
    if (isHost) {
        const std::uint32_t assigned = nextPeerId++;
        connToPeerId[c] = assigned;
        iron::netcubes::writeHello(sendBuf,
                                    iron::netcubes::HelloMsg{assigned});
        transport.send(c,
                       std::span<const std::byte>(sendBuf.data(), sendBuf.size()),
                       iron::SendReliability::Reliable);
    }
});
```

Change to:
```cpp
transport.setOnConnectionOpened([&](iron::ConnectionId c) {
    if (isHost) {
        const std::uint32_t assigned = nextPeerId++;
        connToPeerId[c] = assigned;
        registry.send<iron::netcubes::HelloMsg>(
            c, iron::netcubes::HelloMsg{assigned},
            iron::SendReliability::Reliable);
    }
});
```

Also drop the now-unused `std::vector<std::byte> sendBuf;` declaration earlier in the file.

**Replace the broadcast `writePosition` + raw `transport.send`** — find:
```cpp
if (since >= 33) {
    lastSend = now;
    iron::netcubes::writePosition(sendBuf, iron::netcubes::PositionMsg{
        myId,
        player.position.x, player.position.y, player.position.z});
    std::span<const std::byte> view(sendBuf.data(), sendBuf.size());
    if (isHost) {
        for (const auto& [c, _] : connToPeerId) {
            transport.send(c, view, iron::SendReliability::Unreliable);
        }
    } else {
        transport.send(hostConn, view, iron::SendReliability::Unreliable);
    }
}
```

Change to:
```cpp
if (since >= 33) {
    lastSend = now;
    const iron::netcubes::PositionMsg msg{
        myId,
        player.position.x, player.position.y, player.position.z};
    if (isHost) {
        for (const auto& [c, _] : connToPeerId) {
            registry.send<iron::netcubes::PositionMsg>(
                c, msg, iron::SendReliability::Unreliable);
        }
    } else {
        registry.send<iron::netcubes::PositionMsg>(
            hostConn, msg, iron::SendReliability::Unreliable);
    }
}
```

**Replace the listen-or-connect block** — find:
```cpp
isHost = transport.listen(addr);
if (!isHost) {
    hostConn = transport.connect(addr);
    if (hostConn == iron::kInvalidConnection) {
        iron::Log::error("net-cubes: neither listen nor connect succeeded");
        transport.stop();
        return 1;
    }
}
```

Change to:
```cpp
if (wantsConnect) {
    hostConn = transport.connect(addr);
    if (hostConn == iron::kInvalidConnection) {
        iron::Log::error("net-cubes: connect failed");
        transport.stop();
        return 1;
    }
} else {
    isHost = transport.listen(addr);
    if (!isHost) {
        // No --connect supplied AND listen failed (port taken). Try
        // connect as a fallback — preserves the old "just double-click
        // the exe twice" UX on a single machine.
        hostConn = transport.connect(addr);
        if (hostConn == iron::kInvalidConnection) {
            iron::Log::error("net-cubes: neither listen nor connect succeeded");
            transport.stop();
            return 1;
        }
    }
}
```

- [ ] **Step 3: Delete the now-unused files**

```powershell
Remove-Item games/05-net-cubes/Protocol.h
Remove-Item games/05-net-cubes/Protocol.cpp
Remove-Item tests/test_net_cubes_protocol.cpp
```

- [ ] **Step 4: Update `games/05-net-cubes/CMakeLists.txt`**

Current content includes `Protocol.cpp` in `add_executable`. Replace the file entirely with:

```cmake
add_executable(net-cubes main.cpp)
target_link_libraries(net-cubes PRIVATE ironcore)

# Copy repo-root assets/ next to the built exe so net-cubes finds
# assets/cc0/ground/* at runtime (same pattern as showcase).
add_custom_command(TARGET net-cubes POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:net-cubes>/assets
  COMMENT "Copying CC0 assets next to net-cubes")
```

- [ ] **Step 5: Update `tests/CMakeLists.txt`**

Find and DELETE the `test_net_cubes_protocol` block (the multi-line `add_executable(test_net_cubes_protocol ... )` and the matching `add_test(...)` call).

- [ ] **Step 6: Build + run all tests**

```powershell
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

Use `timeout: 300000`. Expected: clean build; all tests pass. The test count should drop by 1 (removed `test_net_cubes_protocol`) and rise by 2 (added `test_net_args`, `test_message_registry`).

- [ ] **Step 7: Manual smoke test (skip if headless)**

```powershell
./build/games/05-net-cubes/Debug/net-cubes.exe                # window A: host
./build/games/05-net-cubes/Debug/net-cubes.exe --connect 127.0.0.1  # window B: client
```

Move both around; cubes should sync as before. Same behaviour as the M8.2 release.

- [ ] **Step 8: Commit**

```powershell
git add games/05-net-cubes/Messages.h games/05-net-cubes/main.cpp games/05-net-cubes/CMakeLists.txt tests/CMakeLists.txt
git rm games/05-net-cubes/Protocol.h games/05-net-cubes/Protocol.cpp tests/test_net_cubes_protocol.cpp
git commit -m "Net-cubes: refactor onto MessageRegistry + parseNetArgs" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 4: `games/06-net-tag` skeleton (window, scene, chase camera, no networking, no game)

**Files:**
- Create: `games/06-net-tag/CMakeLists.txt`
- Create: `games/06-net-tag/main.cpp`
- Modify: `CMakeLists.txt` (top-level)

Mirrors the M8.2 skeleton-first approach. Same scene as cubes (CC0 ground, sunset skybox, chase camera, single local cube) — networking and game logic land in Tasks 5 and 6. This task locks the renderer wiring before adding gameplay.

- [ ] **Step 1: Create `games/06-net-tag/CMakeLists.txt`**

```cmake
add_executable(net-tag main.cpp)
target_link_libraries(net-tag PRIVATE ironcore)

add_custom_command(TARGET net-tag POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:net-tag>/assets
  COMMENT "Copying CC0 assets next to net-tag")
```

- [ ] **Step 2: Register the exe in top-level `CMakeLists.txt`**

Add after `add_subdirectory(games/05-net-cubes)`:
```cmake
add_subdirectory(games/06-net-tag)
```

- [ ] **Step 3: Create the skeleton `games/06-net-tag/main.cpp`**

This is essentially the M8.2 `games/05-net-cubes/main.cpp` skeleton MINUS networking, with the lit shader source copied verbatim. The single-player skeleton renders one cube and the chase camera works.

Open `games/05-net-cubes/main.cpp` and copy:
- The entire file as a template (it's already a working skeleton + networking).
- KEEP: window + chase camera + scene + cube mesh + ground mesh + skybox + HUD scaffolding.
- DELETE: every networking-related include and code block (transport, registry, peerId state, broadcasts).
- DELETE: the `colorForPeer(...)` cube coloring (use a single hardcoded color for now — it gets replaced in Task 5/6).
- ADD: `int main(int argc, char** argv)` signature even though args aren't used in this task.

The resulting file should be ~250 lines: scene setup, main loop with input + camera + render + HUD. Single cube tracking the player position. HUD shows `"net-tag: skeleton"` placeholder text.

For the cube color in this skeleton, use a hardcoded grey:
```cpp
call.material.emissive = iron::Vec3{0.4f, 0.4f, 0.4f};
```

- [ ] **Step 4: Build**

```powershell
cmake --build build --target net-tag
```

Use `timeout: 300000`. Expected: clean build, asset copy step runs.

- [ ] **Step 5: Manual smoke test (skip if headless)**

```powershell
./build/games/06-net-tag/Debug/net-tag.exe
```

Expected: window opens, sunset sky, ground, single grey cube where you stand, chase camera follows, WASD/QE moves, mouse looks. ESC releases cursor, click to recapture. HUD shows `net-tag: skeleton`.

- [ ] **Step 6: Commit**

```powershell
git add games/06-net-tag/CMakeLists.txt games/06-net-tag/main.cpp CMakeLists.txt
git commit -m "Net-tag: skeleton (window, chase camera, single cube, no networking)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 5: `games/06-net-tag` networking + Messages

**Files:**
- Create: `games/06-net-tag/Messages.h`
- Modify: `games/06-net-tag/main.cpp`

Adds the networking layer to the Task 4 skeleton. Host/client roles via `parseNetArgs`. Multi-cube rendering with peerId color. Position broadcast. HelloMsg + PositionMsg only — game logic (tag swap, scoring, round timer) lands in Task 6.

- [ ] **Step 1: Create `games/06-net-tag/Messages.h`**

```cpp
#pragma once

#include <cstdint>

namespace iron::nettag {

struct HelloMsg {
    static constexpr std::uint8_t kTag = 1;
    std::uint32_t peerId;
};

struct PositionMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t peerId;
    float x, y, z;
};

struct TagSwapMsg {
    static constexpr std::uint8_t kTag = 3;
    std::uint32_t newItPeerId;
};

struct ScoreUpdateMsg {
    static constexpr std::uint8_t kTag = 4;
    std::uint32_t peerId;
    float itTimeSec;
};

struct RoundStartMsg {
    static constexpr std::uint8_t kTag = 5;
    std::uint32_t initialItPeerId;
    float roundDurationSec;
};

struct RoundEndMsg {
    static constexpr std::uint8_t kTag = 6;
    std::uint32_t winnerPeerId;
};

} // namespace iron::nettag
```

(All 6 message types declared up front so Task 6 can use them without revisiting Messages.h.)

- [ ] **Step 2: Add networking to `games/06-net-tag/main.cpp`**

Three additive blocks (no rewrites). Mirror the structure of net-cubes' networking section.

**Add includes** at the top of main.cpp:
```cpp
#include "Messages.h"

#include "core/NetArgs.h"
#include "net/MessageRegistry.h"
#include "net/NetTransport.h"
#include "net/backends/gns/GnsTransport.h"
```

**Update `main()` signature** to take argc/argv:
```cpp
int main(int argc, char** argv) {
```

**Reuse the same `colorForPeer` helper from cubes** — copy the function verbatim from `games/05-net-cubes/main.cpp` into the namespaced block at the top of net-tag's main.cpp:

```cpp
namespace {

iron::Vec3 colorForPeer(std::uint32_t peerId) {
    const float hue = std::fmod(static_cast<float>(peerId) * 0.61803398875f, 1.0f);
    const float s = 0.8f;
    const float v = 0.9f;
    const float c = v * s;
    const float h6 = hue * 6.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(h6, 2.0f) - 1.0f));
    const float m = v - c;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if      (h6 < 1.0f) { r = c; g = x; }
    else if (h6 < 2.0f) { r = x; g = c; }
    else if (h6 < 3.0f) { g = c; b = x; }
    else if (h6 < 4.0f) { g = x; b = c; }
    else if (h6 < 5.0f) { r = x; b = c; }
    else                { r = c; b = x; }
    return iron::Vec3{r + m, g + m, b + m};
}

}  // namespace
```

**Replace the single-cube `CubeState` declaration with the multi-cube map** (same pattern as cubes):
```cpp
struct CubeState { iron::Vec3 displayed; iron::Vec3 target; };
std::unordered_map<std::uint32_t, CubeState> cubes;
constexpr float kCubeSmoothness = 12.0f;
```

**Add networking state + registry setup** (after the scene/HUD setup, before the main loop):
```cpp
    // --- networking ---
    const iron::NetArgs netArgs = iron::parseNetArgs(argc, argv);
    iron::GnsTransport transport;

    bool isHost = false;
    iron::ConnectionId hostConn = iron::kInvalidConnection;
    std::uint32_t myPeerId = 0;
    std::unordered_map<iron::ConnectionId, std::uint32_t> connToPeerId;
    std::uint32_t nextPeerId = 1;

    transport.setOnConnectionOpened([&](iron::ConnectionId c) {
        if (isHost) {
            const std::uint32_t assigned = nextPeerId++;
            connToPeerId[c] = assigned;
            // Note: registry isn't accessible yet (declared below). We
            // forward-declare via shared_ptr OR use a captured reference.
            // Simplest: construct registry BEFORE setting callbacks.
        }
    });

    // ... see correct ordering below
```

Wait — `MessageRegistry` must be constructed before any handler registration, and our `setOnConnectionOpened` lambda needs to call `registry.send<HelloMsg>(...)`. So order is:
1. Construct `MessageRegistry registry(&transport);`
2. Set callbacks (which can capture `registry` by reference)
3. Register typed handlers
4. `transport.start()`
5. listen/connect

Replace the above placeholder with the correct full block:

```cpp
    // --- networking ---
    const iron::NetArgs netArgs = iron::parseNetArgs(argc, argv);
    iron::GnsTransport transport;
    iron::MessageRegistry registry(&transport);

    bool isHost = false;
    iron::ConnectionId hostConn = iron::kInvalidConnection;
    std::uint32_t myPeerId = 0;
    std::unordered_map<iron::ConnectionId, std::uint32_t> connToPeerId;
    std::uint32_t nextPeerId = 1;

    transport.setOnConnectionOpened([&](iron::ConnectionId c) {
        if (isHost) {
            const std::uint32_t assigned = nextPeerId++;
            connToPeerId[c] = assigned;
            registry.send<iron::nettag::HelloMsg>(
                c, iron::nettag::HelloMsg{assigned},
                iron::SendReliability::Reliable);
        }
    });

    transport.setOnConnectionClosed([&](iron::ConnectionId c,
                                         const std::string& reason) {
        if (isHost) {
            auto it = connToPeerId.find(c);
            if (it != connToPeerId.end()) {
                cubes.erase(it->second);
                connToPeerId.erase(it);
            }
        } else {
            iron::Log::warn("net-tag: connection to host closed: %s",
                            reason.c_str());
            glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
        }
    });

    registry.registerHandler<iron::nettag::HelloMsg>(
        [&](iron::ConnectionId /*c*/, const iron::nettag::HelloMsg& msg) {
            if (isHost) {
                iron::Log::warn("net-tag: host received Hello — ignoring");
                return;
            }
            if (myPeerId == 0) myPeerId = msg.peerId;
        });

    registry.registerHandler<iron::nettag::PositionMsg>(
        [&](iron::ConnectionId c, const iron::nettag::PositionMsg& msg) {
            if (isHost) {
                auto it = connToPeerId.find(c);
                if (it == connToPeerId.end() || it->second != msg.peerId) {
                    iron::Log::warn("net-tag: dropping spoofed PositionMsg");
                    return;
                }
            }
            const iron::Vec3 incoming{msg.x, msg.y, msg.z};
            auto [it, inserted] = cubes.try_emplace(msg.peerId);
            if (inserted) it->second.displayed = incoming;
            it->second.target = incoming;
            if (isHost) {
                for (const auto& [otherConn, _] : connToPeerId) {
                    if (otherConn == c) continue;
                    registry.send<iron::nettag::PositionMsg>(
                        otherConn, msg, iron::SendReliability::Unreliable);
                }
            }
        });

    if (!transport.start()) {
        iron::Log::error("net-tag: GnsTransport.start failed");
        return 1;
    }

    if (netArgs.wantsConnect) {
        hostConn = transport.connect(netArgs.addr);
        if (hostConn == iron::kInvalidConnection) {
            iron::Log::error("net-tag: connect failed");
            transport.stop();
            return 1;
        }
    } else {
        isHost = transport.listen(netArgs.addr);
        if (!isHost) {
            hostConn = transport.connect(netArgs.addr);
            if (hostConn == iron::kInvalidConnection) {
                iron::Log::error("net-tag: neither listen nor connect succeeded");
                transport.stop();
                return 1;
            }
        }
    }
```

**Inside the main loop**, after the camera update, add the same per-frame networking work as cubes (poll, update own cube under myId, lerp remote cubes, broadcast position at 30 Hz):

Find the line that updates the single local cube (something like `cubes[0] = CubeState{player.position, player.position};` or wherever Task 4 set it), and REPLACE that whole block with:

```cpp
        transport.poll();

        const std::uint32_t myId = isHost ? 0u : myPeerId;
        const bool haveIdentity = isHost || (myPeerId != 0);
        if (haveIdentity) {
            cubes[myId] = CubeState{player.position, player.position};
        }

        // Lerp remote cubes toward their latest target.
        const float cubeLerp = 1.0f - std::exp(-dt * kCubeSmoothness);
        for (auto& [peerId, cube] : cubes) {
            if (peerId == myId) continue;
            cube.displayed.x += (cube.target.x - cube.displayed.x) * cubeLerp;
            cube.displayed.y += (cube.target.y - cube.displayed.y) * cubeLerp;
            cube.displayed.z += (cube.target.z - cube.displayed.z) * cubeLerp;
        }

        // Broadcast our position ~30 Hz.
        if (haveIdentity) {
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - lastSend).count();
            if (since >= 33) {
                lastSend = now;
                const iron::nettag::PositionMsg msg{
                    myId,
                    player.position.x, player.position.y, player.position.z};
                if (isHost) {
                    for (const auto& [c, _] : connToPeerId) {
                        registry.send<iron::nettag::PositionMsg>(
                            c, msg, iron::SendReliability::Unreliable);
                    }
                } else {
                    registry.send<iron::nettag::PositionMsg>(
                        hostConn, msg, iron::SendReliability::Unreliable);
                }
            }
        }
```

`lastSend` declaration must be added near `prevTime` (before the loop):
```cpp
    auto lastSend = prevTime - std::chrono::milliseconds(33);
```

(The minus-33-ms init is the same fix from M8.2 — don't use `time_point::min()`.)

**Update the cube render loop** to draw all peers using `colorForPeer`:
```cpp
        for (const auto& [peerId, cube] : cubes) {
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = litShader;
            call.model = iron::translation(cube.displayed);
            call.material.texture     = renderer.whiteTexture();
            call.material.normalMap   = renderer.flatNormalTexture();
            call.material.specularMap = renderer.noSpecularTexture();
            call.material.emissive    = colorForPeer(peerId) * 0.4f;
            renderer.submit(call);
        }
```

**Update HUD text** to reflect role:
```cpp
        if (isHost) {
            hud.setText(roleText, "Host (peer 0)");
        } else if (myPeerId != 0) {
            hud.setText(roleText, "Client (peer " + std::to_string(myPeerId) + ")");
        } else {
            hud.setText(roleText, "(connecting...)");
        }
        hud.setText(peersText,
                    "Peers: " + std::to_string(cubes.size()));
```

(`roleText` and `peersText` should already exist from the skeleton's HUD setup — same pattern as cubes.)

**Add `transport.stop()` before `return 0;`** at the end of `main()`.

- [ ] **Step 3: Build**

```powershell
cmake --build build --target net-tag
```

Use `timeout: 180000`. Expected: clean build.

- [ ] **Step 4: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all tests pass (no test regressions).

- [ ] **Step 5: Manual smoke test (skip if headless)**

Window A: `./build/games/06-net-tag/Debug/net-tag.exe` → host.
Window B: `./build/games/06-net-tag/Debug/net-tag.exe --connect 127.0.0.1` → client.
Both windows should show two cubes (one each, distinct colors); moving one should appear in the other; HUD shows role + peer count.

- [ ] **Step 6: Commit**

```powershell
git add games/06-net-tag/Messages.h games/06-net-tag/main.cpp
git commit -m "Net-tag: networking layer (Hello + Position via MessageRegistry)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 6: `games/06-net-tag` gameplay — tag swap, scoring, rounds

**Files:**
- Modify: `games/06-net-tag/main.cpp`

Adds the actual tag gameplay on top of Task 5's networking. Host runs everything (tag detection, "it" handoff, round timer, scoring); clients receive state via 4 new message types.

- [ ] **Step 1: Add per-player score state on host**

After the existing `connToPeerId` declaration, add:
```cpp
    // --- tag game state (host owns; clients are passive renderers) ---
    struct PlayerInfo {
        float itTimeAccumSec = 0.0f;
        float lastTaggedTimeSec = -1.0f;  // for the 0.5s post-swap cooldown
    };
    std::unordered_map<std::uint32_t, PlayerInfo> players;  // peerId -> info

    std::uint32_t itPeerId = 0;  // who is "it" right now (broadcast to clients)
    float roundTimeRemainingSec = 60.0f;
    float lastScoreBroadcastSec = 0.0f;
    bool  roundActive = false;
    float roundEndDisplayUntilSec = 0.0f;
    std::uint32_t winnerPeerId = 0;  // for HUD during round-end display
    float gameElapsedSec = 0.0f;     // monotonically increasing host clock
    constexpr float kTagDistance = 1.5f;
    constexpr float kTagCooldownSec = 0.5f;
    constexpr float kRoundDurationSec = 60.0f;
    constexpr float kScoreBroadcastIntervalSec = 1.0f;
    constexpr float kRoundEndDisplaySec = 5.0f;
```

- [ ] **Step 2: Register handlers for the 4 new client-side message types**

After the existing `registerHandler<HelloMsg>` and `registerHandler<PositionMsg>` blocks, add:

```cpp
    // Client state populated from these host-broadcast events.
    float clientRoundTimeRemainingSec = 0.0f;
    std::unordered_map<std::uint32_t, float> clientScores;  // peerId -> itTimeSec
    bool  clientShowingRoundEnd = false;
    std::uint32_t clientWinnerPeerId = 0;

    registry.registerHandler<iron::nettag::TagSwapMsg>(
        [&](iron::ConnectionId /*c*/, const iron::nettag::TagSwapMsg& msg) {
            itPeerId = msg.newItPeerId;
        });

    registry.registerHandler<iron::nettag::ScoreUpdateMsg>(
        [&](iron::ConnectionId /*c*/, const iron::nettag::ScoreUpdateMsg& msg) {
            clientScores[msg.peerId] = msg.itTimeSec;
        });

    registry.registerHandler<iron::nettag::RoundStartMsg>(
        [&](iron::ConnectionId /*c*/, const iron::nettag::RoundStartMsg& msg) {
            itPeerId = msg.initialItPeerId;
            clientRoundTimeRemainingSec = msg.roundDurationSec;
            clientShowingRoundEnd = false;
            clientScores.clear();
        });

    registry.registerHandler<iron::nettag::RoundEndMsg>(
        [&](iron::ConnectionId /*c*/, const iron::nettag::RoundEndMsg& msg) {
            clientWinnerPeerId = msg.winnerPeerId;
            clientShowingRoundEnd = true;
        });
```

- [ ] **Step 3: Add host-side gameplay tick**

After the `transport.poll()` line in the main loop, BEFORE the broadcast block, add:

```cpp
        // --- host gameplay tick ---
        if (isHost) {
            gameElapsedSec += dt;

            // Ensure host's own player entry exists.
            if (players.find(0) == players.end()) {
                players[0] = PlayerInfo{};
            }
            // Ensure every connected client has a player entry.
            for (const auto& [c, peerId] : connToPeerId) {
                if (players.find(peerId) == players.end()) {
                    players[peerId] = PlayerInfo{};
                }
            }

            if (!roundActive && gameElapsedSec > roundEndDisplayUntilSec) {
                // Start a new round. Reset scores, pick first "it" at random.
                for (auto& [_, info] : players) {
                    info.itTimeAccumSec = 0.0f;
                    info.lastTaggedTimeSec = -1.0f;
                }
                // Pick a random peer to be "it" — including the host.
                std::vector<std::uint32_t> peerList;
                for (const auto& [pid, _] : players) peerList.push_back(pid);
                if (!peerList.empty()) {
                    const std::size_t idx = static_cast<std::size_t>(gameElapsedSec * 1000) % peerList.size();
                    itPeerId = peerList[idx];
                }
                roundTimeRemainingSec = kRoundDurationSec;
                roundActive = true;

                const iron::nettag::RoundStartMsg startMsg{
                    itPeerId, kRoundDurationSec};
                for (const auto& [c, _] : connToPeerId) {
                    registry.send<iron::nettag::RoundStartMsg>(
                        c, startMsg, iron::SendReliability::Reliable);
                }
            }

            if (roundActive) {
                roundTimeRemainingSec -= dt;
                if (auto it = players.find(itPeerId); it != players.end()) {
                    it->second.itTimeAccumSec += dt;
                }

                // Tag check: any non-it player within range + cooldown elapsed → swap.
                const iron::Vec3& itPos = (itPeerId == 0)
                    ? player.position
                    : (cubes.count(itPeerId) ? cubes[itPeerId].target : iron::Vec3{1e9f, 0, 0});
                std::uint32_t newIt = 0;
                bool swap = false;
                for (const auto& [pid, info] : players) {
                    if (pid == itPeerId) continue;
                    const iron::Vec3& pos = (pid == 0)
                        ? player.position
                        : (cubes.count(pid) ? cubes[pid].target : iron::Vec3{1e9f, 0, 0});
                    const float dx = pos.x - itPos.x;
                    const float dy = pos.y - itPos.y;
                    const float dz = pos.z - itPos.z;
                    const float d2 = dx*dx + dy*dy + dz*dz;
                    if (d2 < kTagDistance * kTagDistance &&
                        gameElapsedSec - info.lastTaggedTimeSec > kTagCooldownSec) {
                        newIt = pid;
                        swap = true;
                        break;
                    }
                }
                if (swap) {
                    itPeerId = newIt;
                    players[itPeerId].lastTaggedTimeSec = gameElapsedSec;
                    const iron::nettag::TagSwapMsg swapMsg{itPeerId};
                    for (const auto& [c, _] : connToPeerId) {
                        registry.send<iron::nettag::TagSwapMsg>(
                            c, swapMsg, iron::SendReliability::Reliable);
                    }
                }

                // Broadcast scores every 1 s.
                if (gameElapsedSec - lastScoreBroadcastSec >= kScoreBroadcastIntervalSec) {
                    lastScoreBroadcastSec = gameElapsedSec;
                    for (const auto& [pid, info] : players) {
                        const iron::nettag::ScoreUpdateMsg sMsg{pid, info.itTimeAccumSec};
                        for (const auto& [c, _] : connToPeerId) {
                            registry.send<iron::nettag::ScoreUpdateMsg>(
                                c, sMsg, iron::SendReliability::Reliable);
                        }
                    }
                }

                if (roundTimeRemainingSec <= 0.0f) {
                    // Pick winner: lowest itTimeAccumSec.
                    std::uint32_t winner = 0;
                    float bestTime = 1e9f;
                    for (const auto& [pid, info] : players) {
                        if (info.itTimeAccumSec < bestTime) {
                            bestTime = info.itTimeAccumSec;
                            winner = pid;
                        }
                    }
                    winnerPeerId = winner;
                    roundActive = false;
                    roundEndDisplayUntilSec = gameElapsedSec + kRoundEndDisplaySec;

                    const iron::nettag::RoundEndMsg endMsg{winner};
                    for (const auto& [c, _] : connToPeerId) {
                        registry.send<iron::nettag::RoundEndMsg>(
                            c, endMsg, iron::SendReliability::Reliable);
                    }
                }
            }
        }
```

- [ ] **Step 4: Update cube color to highlight "it"**

In the cube render loop, replace the existing color line with:
```cpp
            iron::Vec3 emissive = colorForPeer(peerId) * 0.4f;
            // Highlight "it" with a strong red boost so it's instantly readable.
            if (peerId == itPeerId) {
                emissive = emissive + iron::Vec3{1.5f, 0.0f, 0.0f};
            }
            call.material.emissive = emissive;
```

- [ ] **Step 5: Update HUD with game-state lines**

Add two more HUD lines after the existing `peersText`:
```cpp
    const iron::HudId stateText = hud.addText(
        "(no round)", iron::Vec2{12, 60}, 2.0f, iron::Vec4{1, 1, 1, 1});
    const iron::HudId scoresText = hud.addText(
        "", iron::Vec2{12, 84}, 1.5f, iron::Vec4{1, 1, 1, 1});
```

In the per-frame HUD update block, REPLACE the existing roleText/peersText lines with:
```cpp
        // role + peer count (unchanged from Task 5)
        if (isHost) {
            hud.setText(roleText, "Host (peer 0)");
        } else if (myPeerId != 0) {
            hud.setText(roleText, "Client (peer " + std::to_string(myPeerId) + ")");
        } else {
            hud.setText(roleText, "(connecting...)");
        }
        hud.setText(peersText,
                    "Peers: " + std::to_string(cubes.size()));

        // game state
        const std::uint32_t myId = isHost ? 0u : myPeerId;
        const float remaining = isHost ? roundTimeRemainingSec
                                        : clientRoundTimeRemainingSec;
        const bool showingEnd = isHost
                                  ? (!roundActive && gameElapsedSec < roundEndDisplayUntilSec)
                                  : clientShowingRoundEnd;
        const std::uint32_t winner = isHost ? winnerPeerId : clientWinnerPeerId;
        if (showingEnd) {
            hud.setText(stateText, "Round over! Winner: peer "
                                    + std::to_string(winner));
        } else if (itPeerId == myId && myId != 0 || (isHost && itPeerId == 0)) {
            const int secs = static_cast<int>(std::max(0.0f, remaining));
            hud.setText(stateText, "You are IT!  " + std::to_string(secs) + "s");
        } else {
            const int secs = static_cast<int>(std::max(0.0f, remaining));
            hud.setText(stateText, "Run!  " + std::to_string(secs) + "s");
        }

        // leaderboard
        const auto& scores = isHost
            ? [&]() -> std::unordered_map<std::uint32_t, float> {
                std::unordered_map<std::uint32_t, float> out;
                for (const auto& [pid, info] : players) out[pid] = info.itTimeAccumSec;
                return out;
              }()
            : clientScores;
        std::vector<std::pair<std::uint32_t, float>> sorted(
            scores.begin(), scores.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
        std::string sb;
        for (const auto& [pid, t] : sorted) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "peer %u: %.1fs   ",
                          static_cast<unsigned>(pid), t);
            sb += buf;
        }
        hud.setText(scoresText, sb);
```

Note: the `isHost ? [&]() { ... }() : clientScores` ternary forces both branches to the same map type. The client side uses `clientScores` directly; the host side materialises a same-shape map from `players`. Slightly wasteful but keeps the rest of the leaderboard code uniform.

- [ ] **Step 6: Apply the client's local round-timer countdown**

Add this AFTER the per-frame networking block, while inside the main loop:
```cpp
        if (!isHost && !clientShowingRoundEnd) {
            clientRoundTimeRemainingSec -= dt;
            if (clientRoundTimeRemainingSec < 0.0f) clientRoundTimeRemainingSec = 0.0f;
        }
```

- [ ] **Step 7: Build**

```powershell
cmake --build build --target net-tag
```

Use `timeout: 180000`. Expected: clean build.

- [ ] **Step 8: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 9: Manual smoke test (skip if headless)**

Launch two instances. Verify the host's HUD ticks down from 60 and starts a round. Cubes move; getting within 1.5m of the "it" cube swaps "it". After 60 seconds, "Round over! Winner: peer N" shows for 5 seconds, then a new round starts with a different initial "it". The "it" cube glows red.

- [ ] **Step 10: Commit**

```powershell
git add games/06-net-tag/main.cpp
git commit -m "Net-tag: gameplay (tag swap, 60s rounds, scoring, red-glow it)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 7: Docs

**Files:**
- Modify: `docs/engine/networking.md`

Add two sections describing the typed dispatcher and the new game.

- [ ] **Step 1: Append to `docs/engine/networking.md`**

Use the Edit tool to add this content after the existing "Play with it: net-cubes" section (or wherever the file currently ends):

```markdown

## Typed messages: `iron::MessageRegistry`

Game code does NOT manually serialise bytes and switch on a tag byte.
That's what the registry is for. Define messages as POD structs with a
1-byte tag:

\`\`\`cpp
struct PositionMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t peerId;
    float x, y, z;
};
\`\`\`

Then wrap a transport once, register handlers per type, and send by type:

\`\`\`cpp
iron::GnsTransport transport;
iron::MessageRegistry registry(&transport);

registry.registerHandler<PositionMsg>([&](iron::ConnectionId c, const PositionMsg& m) {
    cubes[m.peerId] = {m.x, m.y, m.z};
});

// Later:
registry.send<PositionMsg>(connId, PositionMsg{myId, 1.0f, 0.5f, -2.0f},
                            iron::SendReliability::Unreliable);
\`\`\`

Wire format is `[u8 tag][raw memcpy of struct]`. Tag 0 is reserved.
Messages must be trivially copyable (POD only — no \`std::string\`, no
\`std::vector\` inside). Compile-time enforced via `static_assert`. Max
payload is 1200 bytes (one message, one packet, no fragmentation).

Tag namespaces are per-registry — different games can reuse the same tag
values without conflict.

## Play with it: net-tag

\`games/06-net-tag\` — N-player tag with a 60-second round and a live
leaderboard. Built entirely on \`iron::MessageRegistry\`. Cross-machine
LAN play via the shared \`--connect <ip>\` / \`--port <n>\` CLI parsed
by \`iron::parseNetArgs\`.

\`\`\`
net-tag.exe                                # listen on 30005 (host)
net-tag.exe --connect 192.168.1.5          # client connecting to that IP
net-tag.exe --connect 127.0.0.1 --port 40000
\`\`\`

The host is authoritative: detects the tag (distance < 1.5m + 0.5s
cooldown), runs the round timer, owns the scoreboard, broadcasts state.
Clients send position; receive everything else.

Cube colored by peerId via hue rotation; the "it" cube has a strong red
emissive glow so it's instantly readable across the scene.
```

(In the actual file body the triple-backticks are real ` ``` `, not escaped. The escapes above are just so this plan renders cleanly in markdown.)

- [ ] **Step 2: Commit**

```powershell
git add docs/engine/networking.md
git commit -m "Docs: typed dispatcher + net-tag (M8.3)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 8: Code review pass + PR

**Files:** none modified — read-only review (plus fixes the review surfaces)

- [ ] **Step 1: Show the diff range**

```powershell
git log --oneline main..HEAD
git diff main --stat
```

Expected: 7 commits (Tasks 1–7), the files listed in this plan's "File Structure" section.

- [ ] **Step 2: Build + full ctest + smoke-run both new exes**

```powershell
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
./build/games/05-net-cubes/Debug/net-cubes.exe                # smoke (close it after a second)
./build/games/06-net-tag/Debug/net-tag.exe                    # smoke (close it after a second)
```

Use `timeout: 300000` for the build. Expected: clean build; full test pass; both exes launch.

- [ ] **Step 3: Dispatch a code-quality review**

Dispatch `feature-dev:code-reviewer` (or `general-purpose`) with this prompt:

> Review the M8.3 typed-dispatcher + tag changes (`git diff main`) in the Iron Core Engine. Milestone adds `iron::MessageRegistry` (templated typed dispatch over `NetTransport`), `iron::parseNetArgs(argc, argv)` CLI helper, refactors `games/05-net-cubes` onto both (deleting its hand-rolled Protocol), and ships a brand-new `games/06-net-tag` (host-authoritative N-player tag with 60s rounds and scoring). Spec: `docs/superpowers/specs/2026-05-24-typed-dispatcher-and-tag-design.md`.
>
> Focus on:
> 1. **MessageRegistry safety** — destructor unhooks transport; sender returns false on `kInvalidConnection`; wrong-size payload silently dropped; static_asserts catch non-POD types and oversized messages at compile time.
> 2. **Cubes refactor parity** — the refactored `games/05-net-cubes/main.cpp` produces the same observable behaviour as M8.2 (host validates peerId, host rebroadcasts, interpolation works, ESC releases cursor, chase camera).
> 3. **Tag gameplay correctness** — round timer, "it" handoff on collision (with cooldown), scoring accumulates correctly, winner picked correctly, late-joiner handling. Any way two players can both be "it"? Any way the round never ends?
> 4. **parseNetArgs robustness** — handles missing values, malformed IPs and ports; default fallback is safe.
> 5. **Header hygiene** — game code does NOT include any `<steam/...>` headers. Verify via grep.
> 6. **CMake / file changes** — deletions of Protocol.h/.cpp/test correctly applied; tests/CMakeLists.txt entries clean.
>
> Skip style nits. Cap at 10 findings. Under 400 words. End with **APPROVED** or **NEEDS_FIXES**.

- [ ] **Step 4: Address findings**

Apply fixes. Push back on cosmetic suggestions; only block on real correctness issues.

- [ ] **Step 5: Final verification**

```powershell
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, all tests pass.

- [ ] **Step 6: Commit any review fixes (skip if none)**

```powershell
git add -A
git commit -m "M8.3: address code-review findings" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

- [ ] **Step 7: Push and open the PR**

```powershell
git push -u origin feat/typed-dispatcher-and-tag
gh pr create --title "Milestone 8.3: Typed message dispatcher + tag game" --body "$(cat <<'EOF'
## Summary

Two deliverables in one PR:

1. **`iron::MessageRegistry`** — templated typed-message dispatch over `NetTransport`. Game code defines POD message structs with `static constexpr std::uint8_t kTag` and uses `registry.registerHandler<Msg>(...)` / `registry.send<Msg>(...)`. Wire format `[u8 tag][raw memcpy]`. Compile-time enforced via `static_assert` (POD only, tag != 0, max 1200 bytes).

2. **`games/06-net-tag`** — brand-new N-player tag game built on the dispatcher. Host-authoritative: detects collisions (< 1.5m + 0.5s cooldown), runs 60s round timer, owns scoreboard. "It" cube glows red. Live leaderboard in HUD. LAN cross-machine play via shared `iron::parseNetArgs(argc, argv)` (`--connect <ip>` / `--port <n>`).

`games/05-net-cubes` refactored onto the same dispatcher (`Protocol.h/.cpp` and its unit test deleted). Both demos now share the same networking style — proves the abstraction works for the existing scenario and the new one.

## Test plan

- [x] `test_net_args` passes (8 cases: defaults, --connect, --port, both, missing value, malformed inputs, unknown flag)
- [x] `test_message_registry` passes (5 cases: round-trip per type, wrong-size dropped, unregistered dropped, multi-msg-one-poll, send-on-invalid-conn)
- [x] All unit tests pass
- [x] `net-cubes.exe` and `net-cubes.exe --connect 127.0.0.1` still play the M8.2 demo correctly
- [x] `net-tag.exe` two-process play: rounds start, "it" swaps on collision, scores accumulate, winner announced after 60s, new round starts
- [x] No `games/*/*.cpp` includes `<steam/...>` (header hygiene preserved)

## Out of scope (M8.4+)

- Snapshot interpolation tuning beyond cubes' simple lerp
- Client-side prediction / lag compensation
- Property replication / "network variables"
- Reconnect / host migration
- NAT traversal, Steam Datagram Relay
- Length-prefixed string/array messages
- Strandbound multiplayer integration

EOF
)"
```

Return the PR URL.

---

## Self-review (run after writing the plan, before handoff)

Already done inline:

- **Spec coverage:**
  - `iron::MessageRegistry` with kTag + static_asserts → Task 2
  - `MessageRegistry` test suite (5 cases per spec) → Task 2 (Step 1)
  - `iron::parseNetArgs(argc, argv)` → Task 1
  - `parseNetArgs` test suite (defaults, --connect, --port, edge cases) → Task 1 (Step 1)
  - Cubes refactor onto registry + parseNetArgs + delete old Protocol → Task 3
  - net-tag skeleton (window/scene/chase camera) → Task 4
  - net-tag networking + Messages.h (6 types) → Task 5
  - net-tag gameplay (tag swap + cooldown + 60s round + scoring + "it" red glow + leaderboard + round-end display) → Task 6
  - Late-joiner snapshot (`RoundStartMsg` carries `roundDurationSec` so late joiners see same clock — actually our Task 6 sends round duration but not remaining time; late joiners get full duration. Acceptable per spec's "tiny advantage" note.)
  - Docs update → Task 7
  - Code review pass → Task 8
- **Non-goals respected:** no prediction, no replication, no reconnect, no NAT punching, no variable-length messages, no Strandbound integration.
- **Placeholder scan:** no TBD/TODO. Triple-backticks in Task 7 are escaped only in this plan rendering — the doc file gets real backticks, and Task 7 says that explicitly.
- **Type consistency:** `iron::nettag::HelloMsg`, `PositionMsg`, `TagSwapMsg`, `ScoreUpdateMsg`, `RoundStartMsg`, `RoundEndMsg` used uniformly. `iron::MessageRegistry` API consistent across header, impl, tests, and both game usages.
