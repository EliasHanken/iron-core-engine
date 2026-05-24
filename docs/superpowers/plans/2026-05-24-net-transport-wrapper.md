# Engine Networking Wrapper (M8.1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `iron::NetTransport` abstract base + `iron::GnsTransport` (production) and `iron::MockTransport` (test-only) implementations under `engine/net/`, then rewrite `games/04-net-pingpong/main.cpp` on top of the wrapper so game code never touches GNS headers.

**Architecture:** Mirrors the renderer pattern (`engine/render/Renderer.h` abstract base + `engine/render/backends/opengl/OpenGLRenderer.{h,cpp}` concrete impl). `engine/net/NetTransport.h` defines the interface and types (`NetAddress`, `ConnectionId`, `SendReliability`); `engine/net/backends/gns/GnsTransport` wraps GameNetworkingSockets; `engine/net/backends/mock/MockTransport` provides in-memory loopback for unit tests. `ironcore` PUBLIC-links GNS so games inherit it transitively (same pattern as glfw/glad).

**Tech Stack:** C++23, MSVC, `std::function`, `std::span<const std::byte>`, custom CTest harness, GameNetworkingSockets (from vcpkg).

**Spec:** `docs/superpowers/specs/2026-05-24-net-transport-wrapper-design.md`

---

## File Structure

**New files:**
- `engine/net/NetTransport.h` — abstract base; `NetAddress`, `ConnectionId`, `SendReliability`
- `engine/net/NetTransport.cpp` — minimal: virtual dtor anchor (avoid weak vtable warnings) + small inline-friendly helpers
- `engine/net/backends/gns/GnsTransport.h` + `.cpp` — GNS implementation
- `engine/net/backends/mock/MockTransport.h` + `.cpp` — paired in-memory implementation; lives in engine (not tests) so it's available as a reference impl and to future "offline mode" games
- `tests/test_mock_net_transport.cpp` — covers the 6 contract cases from the spec
- `docs/engine/networking.md` — short intro + the architecture sketch + the driving-model bullets

**Modified files:**
- `engine/CMakeLists.txt` — add 4 new `.cpp` sources; `target_link_libraries(ironcore PUBLIC ... GameNetworkingSockets::GameNetworkingSockets)`
- `games/04-net-pingpong/CMakeLists.txt` — drop direct GNS link (now transitive via ironcore); link `ironcore`
- `games/04-net-pingpong/main.cpp` — rewrite on `iron::GnsTransport` (~80 lines)
- `tests/CMakeLists.txt` — register `test_mock_net_transport`

---

## Task 0: Branch setup

**Files:** none (git state only)

`main` is protected — work goes on a feature branch.

- [ ] **Step 1: Create and switch to the feature branch**

```powershell
git checkout -b feat/net-transport-wrapper
git status
```

Expected: `On branch feat/net-transport-wrapper`, clean tree (modulo the same CRLF/LF warnings on a few docs files we've seen all session — those are harmless).

---

## Task 1: Abstract `NetTransport` interface + common types

**Files:**
- Create: `engine/net/NetTransport.h`
- Create: `engine/net/NetTransport.cpp`
- Modify: `engine/CMakeLists.txt`

This task does not include any implementation — just the pure-virtual abstract base and the types that game code uses. Nothing to test directly (no behaviour), but the file must compile cleanly when added to `ironcore`.

- [ ] **Step 1: Create `engine/net/NetTransport.h`**

```cpp
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace iron {

// Opaque connection handle. Stable for the lifetime of the connection
// within a single NetTransport instance. Never reused after close.
// kInvalidConnection (== 0) is the never-valid sentinel.
using ConnectionId = std::uint32_t;
constexpr ConnectionId kInvalidConnection = 0;

// Network endpoint. IPv4 only today; IPv6 / hostnames are future.
// `ipv4` is stored as a host-order 32-bit value (so 127.0.0.1 == 0x7F000001).
struct NetAddress {
    std::uint32_t ipv4 = 0x7F000001;  // 127.0.0.1 default
    std::uint16_t port = 0;

    friend bool operator==(const NetAddress&, const NetAddress&) = default;
};

enum class SendReliability {
    Reliable,    // retransmit + in-order
    Unreliable,  // best-effort, no ordering
};

// Transport-agnostic networking interface. One concrete implementation
// today (GnsTransport over GameNetworkingSockets); MockTransport exists
// for tests. Game code holds a NetTransport* / unique_ptr<NetTransport>
// and never includes GNS headers.
//
// Driving model:
//   1. Construct a concrete transport.
//   2. Install observer callbacks via the setOn... methods.
//   3. start() the transport.
//   4. listen(addr) and/or connect(addr) (a server may also act as a client).
//   5. Each game-loop tick, call poll(). The transport fires the observer
//      callbacks for any state changes / incoming messages.
//   6. send() to push bytes; close() to drop a connection; stop() to shut down.
class NetTransport {
public:
    virtual ~NetTransport();

    // --- lifecycle ---
    // Initialise the underlying transport. Returns false on failure.
    // Idempotent: start() after a successful start() is a no-op and returns true.
    virtual bool start() = 0;

    // Shut down. Closes every open connection without firing onConnectionClosed
    // for them (caller is shutting down on purpose). Idempotent.
    virtual void stop() = 0;

    // --- endpoints ---
    // Bind a listener at `addr`. Returns false on failure (port taken,
    // bad address, transport not started). New clients arrive via
    // onConnectionOpened.
    virtual bool listen(NetAddress addr) = 0;

    // Initiate a connection to `addr`. Returns a ConnectionId immediately.
    // The connection is NOT open yet — onConnectionOpened fires when the
    // handshake completes, or onConnectionClosed fires if it fails.
    // Returns kInvalidConnection on synchronous failure.
    virtual ConnectionId connect(NetAddress addr) = 0;

    // --- I/O ---
    // Send a byte buffer on `conn`. The buffer is copied internally —
    // caller can free immediately after return.
    // Returns false if `conn` is unknown or already closed (no callback
    // fires in that case).
    virtual bool send(ConnectionId conn,
                      std::span<const std::byte> bytes,
                      SendReliability reliability) = 0;

    // Close one connection. The remote side will see onConnectionClosed
    // on its next poll(). The local onConnectionClosed does NOT fire
    // for connections closed via this method (caller already knows).
    virtual void close(ConnectionId conn) = 0;

    // Drive the transport: dispatch status callbacks, drain inbound
    // message queues. Call once per game-loop tick on the same thread
    // that constructs the transport. Callbacks fire on the calling thread.
    virtual void poll() = 0;

    // --- observers ---
    // Set once after construction, before start() (later calls replace
    // the previous callback; older callbacks are discarded).
    using OnConnectionOpenedFn = std::function<void(ConnectionId)>;
    using OnConnectionClosedFn = std::function<void(ConnectionId,
                                                     const std::string& reason)>;
    using OnMessageFn          = std::function<void(ConnectionId,
                                                     std::span<const std::byte>)>;

    virtual void setOnConnectionOpened(OnConnectionOpenedFn) = 0;
    virtual void setOnConnectionClosed(OnConnectionClosedFn) = 0;
    virtual void setOnMessage(OnMessageFn) = 0;
};

} // namespace iron
```

- [ ] **Step 2: Create `engine/net/NetTransport.cpp`**

```cpp
#include "net/NetTransport.h"

namespace iron {

// Out-of-line virtual destructor anchor: keeps the vtable in this
// translation unit instead of every consumer. Body is empty by design.
NetTransport::~NetTransport() = default;

} // namespace iron
```

- [ ] **Step 3: Wire CMake**

Edit `engine/CMakeLists.txt`. Add `net/NetTransport.cpp` to the `add_library(ironcore STATIC ...)` source list. Insert alphabetically — after `core/Window.cpp` and before `physics/Rope.cpp` (so the section reads `core/`, then `net/`, then `physics/`, then `render/`, then `scene/`, then `ui/`).

Concrete placement:
```cmake
add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Input.cpp
  core/Platform.cpp
  core/Application.cpp
  net/NetTransport.cpp
  scene/Mesh.cpp
  ...
```

- [ ] **Step 4: Build to confirm it compiles cleanly**

```powershell
cmake --build build --target ironcore
```

Use timeout: 180000. Expected: clean build, no warnings about the new file.

- [ ] **Step 5: Commit**

```powershell
git add engine/net/NetTransport.h engine/net/NetTransport.cpp engine/CMakeLists.txt
git commit -m "Engine: NetTransport abstract base + common types" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: `MockTransport` (TDD)

**Files:**
- Create: `engine/net/backends/mock/MockTransport.h`
- Create: `engine/net/backends/mock/MockTransport.cpp`
- Create: `tests/test_mock_net_transport.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

`MockTransport` is pure C++ (no GNS), so we implement it test-first. Two paired instances connect by finding each other via a static in-process registry keyed on the listen address.

- [ ] **Step 1: Write the failing test**

Create `tests/test_mock_net_transport.cpp`:

```cpp
#include "test_framework.h"
#include "net/NetTransport.h"
#include "net/backends/mock/MockTransport.h"

#include <cstring>
#include <string>
#include <vector>

using namespace iron;

namespace {

// Helper: re-interpret a string literal as a byte span.
std::span<const std::byte> asBytes(std::string_view sv) {
    return {reinterpret_cast<const std::byte*>(sv.data()), sv.size()};
}

std::string asString(std::span<const std::byte> b) {
    return std::string{reinterpret_cast<const char*>(b.data()), b.size()};
}

}  // namespace

int main() {
    constexpr NetAddress kAddr{0x7F000001, 1234};

    // Case 1: paired connect, both sides see onConnectionOpened.
    {
        MockTransport server, client;
        bool serverOpened = false, clientOpened = false;
        ConnectionId serverSawConn = kInvalidConnection;
        ConnectionId clientSawConn = kInvalidConnection;

        server.setOnConnectionOpened([&](ConnectionId c) {
            serverOpened = true;
            serverSawConn = c;
        });
        client.setOnConnectionOpened([&](ConnectionId c) {
            clientOpened = true;
            clientSawConn = c;
        });

        CHECK(server.start());
        CHECK(server.listen(kAddr));
        CHECK(client.start());

        const ConnectionId clientConn = client.connect(kAddr);
        CHECK(clientConn != kInvalidConnection);

        server.poll();
        client.poll();

        CHECK(serverOpened);
        CHECK(clientOpened);
        CHECK(serverSawConn != kInvalidConnection);
        CHECK(clientSawConn == clientConn);
    }

    // Case 2: send delivers via onMessage on the peer.
    {
        MockTransport server, client;
        std::string receivedOnServer;

        server.setOnMessage([&](ConnectionId, std::span<const std::byte> b) {
            receivedOnServer = asString(b);
        });
        CHECK(server.start());
        CHECK(server.listen(kAddr));
        CHECK(client.start());
        const ConnectionId clientConn = client.connect(kAddr);
        server.poll();
        client.poll();

        CHECK(client.send(clientConn, asBytes("hello"), SendReliability::Reliable));
        server.poll();

        CHECK(receivedOnServer == "hello");
    }

    // Case 3: reliable preserves in-order delivery for multiple sends.
    {
        MockTransport server, client;
        std::vector<std::string> received;
        server.setOnMessage([&](ConnectionId, std::span<const std::byte> b) {
            received.push_back(asString(b));
        });
        CHECK(server.start());
        CHECK(server.listen(kAddr));
        CHECK(client.start());
        const ConnectionId clientConn = client.connect(kAddr);
        server.poll();
        client.poll();

        client.send(clientConn, asBytes("one"),   SendReliability::Reliable);
        client.send(clientConn, asBytes("two"),   SendReliability::Reliable);
        client.send(clientConn, asBytes("three"), SendReliability::Reliable);
        server.poll();

        CHECK(received.size() == 3);
        CHECK(received[0] == "one");
        CHECK(received[1] == "two");
        CHECK(received[2] == "three");
    }

    // Case 4: close() fires onConnectionClosed on peer (NOT on caller).
    {
        MockTransport server, client;
        bool serverClosed = false;
        bool clientClosed = false;
        server.setOnConnectionClosed([&](ConnectionId, const std::string&) {
            serverClosed = true;
        });
        client.setOnConnectionClosed([&](ConnectionId, const std::string&) {
            clientClosed = true;
        });
        CHECK(server.start());
        CHECK(server.listen(kAddr));
        CHECK(client.start());
        const ConnectionId clientConn = client.connect(kAddr);
        server.poll();
        client.poll();

        client.close(clientConn);
        server.poll();
        client.poll();

        CHECK(serverClosed);
        CHECK(!clientClosed);  // local caller asked for the close
    }

    // Case 5: send on closed connection returns false; no callback, no crash.
    {
        MockTransport server, client;
        int messageCount = 0;
        server.setOnMessage([&](ConnectionId, std::span<const std::byte>) {
            ++messageCount;
        });
        CHECK(server.start());
        CHECK(server.listen(kAddr));
        CHECK(client.start());
        const ConnectionId clientConn = client.connect(kAddr);
        server.poll();
        client.poll();

        client.close(clientConn);

        CHECK(!client.send(clientConn, asBytes("x"), SendReliability::Reliable));
        server.poll();
        CHECK(messageCount == 0);
    }

    // Case 6: send on invalid id returns false.
    {
        MockTransport t;
        CHECK(t.start());
        CHECK(!t.send(kInvalidConnection, asBytes("x"), SendReliability::Reliable));
        CHECK(!t.send(99999, asBytes("x"), SendReliability::Reliable));
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Run the test to verify it fails to compile**

```powershell
cmake --build build --target test_mock_net_transport
```

Expected: compile error — `'MockTransport.h': No such file or directory` (or similar).

- [ ] **Step 3: Create `engine/net/backends/mock/MockTransport.h`**

```cpp
#pragma once

#include "net/NetTransport.h"

#include <cstddef>
#include <functional>
#include <queue>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace iron {

// In-process loopback NetTransport for unit tests. All MockTransport
// instances in the same process discover each other via a static registry
// keyed on listen address; calling connect(addr) finds the listening
// peer (if any) and creates paired ConnectionIds on both sides.
//
// Reliability is NOT modelled — every send delivers in FIFO order.
class MockTransport : public NetTransport {
public:
    MockTransport();
    ~MockTransport() override;

    bool start() override;
    void stop() override;

    bool listen(NetAddress addr) override;
    ConnectionId connect(NetAddress addr) override;

    bool send(ConnectionId conn,
              std::span<const std::byte> bytes,
              SendReliability reliability) override;

    void close(ConnectionId conn) override;
    void poll() override;

    void setOnConnectionOpened(OnConnectionOpenedFn fn) override { onOpened_ = std::move(fn); }
    void setOnConnectionClosed(OnConnectionClosedFn fn) override { onClosed_ = std::move(fn); }
    void setOnMessage(OnMessageFn fn) override          { onMessage_ = std::move(fn); }

private:
    enum class EventType { Opened, Closed, Message };

    struct PendingEvent {
        EventType            type;
        ConnectionId         conn;
        std::vector<std::byte> bytes;   // populated for Message
        std::string          reason;    // populated for Closed
    };

    struct PeerLink {
        MockTransport* peer;
        ConnectionId   peerConn;
    };

    // Receive an event from a peer; called only by peer instances.
    void enqueueFromPeer(PendingEvent ev);
    void unregisterConnection(ConnectionId conn);

    bool started_ = false;
    bool listening_ = false;
    NetAddress listenAddr_{};

    std::unordered_map<ConnectionId, PeerLink> connections_;
    ConnectionId nextId_ = 1;

    std::queue<PendingEvent> inbox_;

    OnConnectionOpenedFn onOpened_;
    OnConnectionClosedFn onClosed_;
    OnMessageFn          onMessage_;
};

} // namespace iron
```

- [ ] **Step 4: Create `engine/net/backends/mock/MockTransport.cpp`**

```cpp
#include "net/backends/mock/MockTransport.h"

#include <algorithm>
#include <vector>

namespace iron {

namespace {

// Process-wide registry of all live MockTransport instances. Lets
// connect(addr) find a listening peer the same way a real network does
// (a server bound a port, a client picks the same address). Not
// thread-safe — tests are single-threaded.
std::vector<MockTransport*>& registry() {
    static std::vector<MockTransport*> g_mocks;
    return g_mocks;
}

}  // namespace

MockTransport::MockTransport() {
    registry().push_back(this);
}

MockTransport::~MockTransport() {
    stop();
    auto& reg = registry();
    reg.erase(std::remove(reg.begin(), reg.end(), this), reg.end());
}

bool MockTransport::start() {
    started_ = true;
    return true;
}

void MockTransport::stop() {
    if (!started_) return;
    // Drop all connections without firing local onClosed (per the
    // NetTransport contract — stop is a deliberate local shutdown).
    // Peers, however, do see a Closed event so their callbacks fire.
    for (auto& [id, link] : connections_) {
        if (link.peer) {
            link.peer->enqueueFromPeer({EventType::Closed, link.peerConn, {}, "peer stopped"});
            link.peer->unregisterConnection(link.peerConn);
        }
    }
    connections_.clear();
    listening_ = false;
    started_ = false;
    // Drain our own inbox — we are not going to poll() it.
    std::queue<PendingEvent> empty;
    inbox_.swap(empty);
}

bool MockTransport::listen(NetAddress addr) {
    if (!started_) return false;
    listenAddr_ = addr;
    listening_ = true;
    return true;
}

ConnectionId MockTransport::connect(NetAddress addr) {
    if (!started_) return kInvalidConnection;
    // Find a peer that is listening on `addr`.
    MockTransport* peer = nullptr;
    for (MockTransport* candidate : registry()) {
        if (candidate != this && candidate->started_ &&
            candidate->listening_ && candidate->listenAddr_ == addr) {
            peer = candidate;
            break;
        }
    }
    if (!peer) return kInvalidConnection;

    const ConnectionId myId   = nextId_++;
    const ConnectionId peerId = peer->nextId_++;
    connections_[myId]            = {peer, peerId};
    peer->connections_[peerId]    = {this, myId};

    // Enqueue opened events on both sides; both will fire on next poll().
    inbox_.push({EventType::Opened, myId, {}, {}});
    peer->enqueueFromPeer({EventType::Opened, peerId, {}, {}});

    return myId;
}

bool MockTransport::send(ConnectionId conn,
                         std::span<const std::byte> bytes,
                         SendReliability /*reliability*/) {
    if (conn == kInvalidConnection) return false;
    auto it = connections_.find(conn);
    if (it == connections_.end()) return false;
    if (!it->second.peer) return false;
    std::vector<std::byte> copy(bytes.begin(), bytes.end());
    it->second.peer->enqueueFromPeer(
        {EventType::Message, it->second.peerConn, std::move(copy), {}});
    return true;
}

void MockTransport::close(ConnectionId conn) {
    auto it = connections_.find(conn);
    if (it == connections_.end()) return;
    if (it->second.peer) {
        it->second.peer->enqueueFromPeer(
            {EventType::Closed, it->second.peerConn, {}, "peer closed"});
        it->second.peer->unregisterConnection(it->second.peerConn);
    }
    connections_.erase(it);
}

void MockTransport::poll() {
    // Move to a local copy so callbacks that issue more sends (and
    // enqueue more events) don't interfere with the iteration.
    std::queue<PendingEvent> local;
    local.swap(inbox_);
    while (!local.empty()) {
        PendingEvent& ev = local.front();
        switch (ev.type) {
            case EventType::Opened:
                if (onOpened_) onOpened_(ev.conn);
                break;
            case EventType::Closed:
                if (onClosed_) onClosed_(ev.conn, ev.reason);
                unregisterConnection(ev.conn);
                break;
            case EventType::Message:
                if (onMessage_) {
                    onMessage_(ev.conn,
                               std::span<const std::byte>(ev.bytes.data(),
                                                          ev.bytes.size()));
                }
                break;
        }
        local.pop();
    }
}

void MockTransport::enqueueFromPeer(PendingEvent ev) {
    inbox_.push(std::move(ev));
}

void MockTransport::unregisterConnection(ConnectionId conn) {
    connections_.erase(conn);
}

} // namespace iron
```

- [ ] **Step 5: Wire CMake**

Edit `engine/CMakeLists.txt`. Add `net/backends/mock/MockTransport.cpp` right after `net/NetTransport.cpp` (so the net/ block stays grouped):

```cmake
  net/NetTransport.cpp
  net/backends/mock/MockTransport.cpp
```

Edit `tests/CMakeLists.txt`. Add after the last existing `iron_add_test(...)` line (currently `test_reflection` is the bottom of the simple ones; the texture_loader / free_fly_camera ones from M7 come after it):

```cmake
iron_add_test(test_mock_net_transport test_mock_net_transport.cpp)
```

- [ ] **Step 6: Build + run the test**

```powershell
cmake --build build --target test_mock_net_transport
ctest --test-dir build -C Debug -R test_mock_net_transport --output-on-failure
```

Use timeout: 180000 on the build. Expected: `OK - all checks passed`, CTest reports PASS.

- [ ] **Step 7: Commit**

```powershell
git add engine/net/backends/mock tests/test_mock_net_transport.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "Engine: MockTransport (in-process loopback for tests)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: `GnsTransport` (GameNetworkingSockets backend)

**Files:**
- Create: `engine/net/backends/gns/GnsTransport.h`
- Create: `engine/net/backends/gns/GnsTransport.cpp`
- Modify: `engine/CMakeLists.txt` (add source; link GNS PUBLIC into ironcore)

This task is implementation-focused — we already have integration coverage via the existing ping-pong exe (which Task 4 will rewrite onto this class). Detailed unit tests against a real GNS instance would require a network harness — out of scope. Correctness here is validated by Task 4's smoke test.

- [ ] **Step 1: Create `engine/net/backends/gns/GnsTransport.h`**

```cpp
#pragma once

#include "net/NetTransport.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>

// Forward-declarations only: do NOT include any <steam/...> headers here,
// so client code stays free of GNS headers.
struct ISteamNetworkingSockets;
using HSteamListenSocket   = std::uint32_t;
using HSteamNetConnection  = std::uint32_t;
using HSteamNetPollGroup   = std::uint32_t;

namespace iron {

// NetTransport implemented on Valve's GameNetworkingSockets.
//
// Owns one ISteamNetworkingSockets singleton interface, one poll group
// (used for all accepted connections), and a translation table between
// GNS handles (HSteamNetConnection) and engine-side ConnectionIds.
//
// One GnsTransport per process — GameNetworkingSockets_Init/Kill are
// process-global lifecycle. Constructing two GnsTransport instances
// simultaneously is undefined.
class GnsTransport : public NetTransport {
public:
    GnsTransport();
    ~GnsTransport() override;

    bool start() override;
    void stop() override;

    bool listen(NetAddress addr) override;
    ConnectionId connect(NetAddress addr) override;

    bool send(ConnectionId conn,
              std::span<const std::byte> bytes,
              SendReliability reliability) override;

    void close(ConnectionId conn) override;
    void poll() override;

    void setOnConnectionOpened(OnConnectionOpenedFn fn) override { onOpened_ = std::move(fn); }
    void setOnConnectionClosed(OnConnectionClosedFn fn) override { onClosed_ = std::move(fn); }
    void setOnMessage(OnMessageFn fn) override          { onMessage_ = std::move(fn); }

private:
    // Static thunk for the GNS C-style status-change callback. Recovers
    // the GnsTransport* from the connection's user-data slot we set on
    // every accept/connect.
    static void statusChangedThunk(void* info);

    // Member dispatcher; called by statusChangedThunk.
    void handleStatusChanged(HSteamNetConnection h, int newState);

    ConnectionId registerConnection(HSteamNetConnection h);
    void         unregisterConnection(ConnectionId conn);
    HSteamNetConnection lookup(ConnectionId conn) const;

    bool started_ = false;
    ISteamNetworkingSockets* sockets_ = nullptr;
    HSteamListenSocket  listenSocket_ = 0;   // 0 == k_HSteamListenSocket_Invalid
    HSteamNetPollGroup  pollGroup_    = 0;   // 0 == k_HSteamNetPollGroup_Invalid

    std::unordered_map<ConnectionId, HSteamNetConnection> idToHandle_;
    std::unordered_map<HSteamNetConnection, ConnectionId> handleToId_;
    ConnectionId nextId_ = 1;

    OnConnectionOpenedFn onOpened_;
    OnConnectionClosedFn onClosed_;
    OnMessageFn          onMessage_;
};

} // namespace iron
```

- [ ] **Step 2: Create `engine/net/backends/gns/GnsTransport.cpp`**

```cpp
#include "net/backends/gns/GnsTransport.h"

#include "core/Log.h"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

#include <cstring>

namespace iron {

namespace {

constexpr std::int64_t kUserDataMarker = 0xC0DE'C0DE'C0DE'C0DEll;

// We squash our `this` pointer into the GNS per-connection user-data
// slot (int64) so the static status thunk can recover us. The marker
// helps spot mis-uses if someone repurposes user-data later.
std::int64_t packThisPointer(void* p) {
    return reinterpret_cast<std::int64_t>(p);
}

GnsTransport* unpackThisPointer(std::int64_t v) {
    return reinterpret_cast<GnsTransport*>(v);
}

}  // namespace

GnsTransport::GnsTransport() = default;

GnsTransport::~GnsTransport() {
    stop();
}

bool GnsTransport::start() {
    if (started_) return true;

    SteamNetworkingErrMsg errMsg{};
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        Log::error("GnsTransport: init failed: %s", errMsg);
        return false;
    }

    sockets_ = SteamNetworkingSockets();
    if (!sockets_) {
        Log::error("GnsTransport: SteamNetworkingSockets() returned null");
        GameNetworkingSockets_Kill();
        return false;
    }

    pollGroup_ = sockets_->CreatePollGroup();
    if (pollGroup_ == k_HSteamNetPollGroup_Invalid) {
        Log::error("GnsTransport: CreatePollGroup failed");
        sockets_ = nullptr;
        GameNetworkingSockets_Kill();
        return false;
    }

    started_ = true;
    return true;
}

void GnsTransport::stop() {
    if (!started_) return;

    // Close every tracked connection without firing local onClosed.
    for (auto& [id, h] : idToHandle_) {
        sockets_->CloseConnection(h, 0, "transport stopped", false);
    }
    idToHandle_.clear();
    handleToId_.clear();

    if (pollGroup_ != k_HSteamNetPollGroup_Invalid) {
        sockets_->DestroyPollGroup(pollGroup_);
        pollGroup_ = k_HSteamNetPollGroup_Invalid;
    }
    if (listenSocket_ != k_HSteamListenSocket_Invalid) {
        sockets_->CloseListenSocket(listenSocket_);
        listenSocket_ = k_HSteamListenSocket_Invalid;
    }

    sockets_ = nullptr;
    GameNetworkingSockets_Kill();
    started_ = false;
}

namespace {

// Build a SteamNetworkingConfigValue_t array that installs both
// the connection-status callback AND a per-connection user-data slot
// pointing at the owning GnsTransport. `out` must outlive the call
// that consumes it.
int makeOptions(GnsTransport* self,
                SteamNetworkingConfigValue_t (&out)[2]) {
    out[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                  reinterpret_cast<void*>(&GnsTransport::statusChangedThunk));
    out[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData,
                    packThisPointer(self));
    return 2;
}

}  // namespace

bool GnsTransport::listen(NetAddress addr) {
    if (!started_) return false;

    SteamNetworkingIPAddr sa;
    sa.Clear();
    sa.SetIPv4(addr.ipv4, addr.port);

    SteamNetworkingConfigValue_t opts[2];
    const int n = makeOptions(this, opts);
    HSteamListenSocket s = sockets_->CreateListenSocketIP(sa, n, opts);
    if (s == k_HSteamListenSocket_Invalid) {
        Log::error("GnsTransport: CreateListenSocketIP failed");
        return false;
    }
    // One listen socket per transport for now. Closing the old one if
    // listen() is called again keeps the contract simple.
    if (listenSocket_ != k_HSteamListenSocket_Invalid) {
        sockets_->CloseListenSocket(listenSocket_);
    }
    listenSocket_ = s;
    return true;
}

ConnectionId GnsTransport::connect(NetAddress addr) {
    if (!started_) return kInvalidConnection;

    SteamNetworkingIPAddr sa;
    sa.Clear();
    sa.SetIPv4(addr.ipv4, addr.port);

    SteamNetworkingConfigValue_t opts[2];
    const int n = makeOptions(this, opts);
    HSteamNetConnection h = sockets_->ConnectByIPAddress(sa, n, opts);
    if (h == k_HSteamNetConnection_Invalid) {
        Log::error("GnsTransport: ConnectByIPAddress failed");
        return kInvalidConnection;
    }
    return registerConnection(h);
}

bool GnsTransport::send(ConnectionId conn,
                        std::span<const std::byte> bytes,
                        SendReliability reliability) {
    if (!started_ || conn == kInvalidConnection) return false;
    auto it = idToHandle_.find(conn);
    if (it == idToHandle_.end()) return false;
    const int flags = (reliability == SendReliability::Reliable)
                          ? k_nSteamNetworkingSend_Reliable
                          : k_nSteamNetworkingSend_Unreliable;
    const EResult r = sockets_->SendMessageToConnection(
        it->second, bytes.data(),
        static_cast<std::uint32_t>(bytes.size()), flags, nullptr);
    return r == k_EResultOK;
}

void GnsTransport::close(ConnectionId conn) {
    auto it = idToHandle_.find(conn);
    if (it == idToHandle_.end()) return;
    sockets_->CloseConnection(it->second, 0, "local close", false);
    unregisterConnection(conn);
}

void GnsTransport::poll() {
    if (!started_) return;
    sockets_->RunCallbacks();

    SteamNetworkingMessage_t* msgs[16];
    while (true) {
        const int n = sockets_->ReceiveMessagesOnPollGroup(pollGroup_, msgs, 16);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            SteamNetworkingMessage_t* m = msgs[i];
            auto it = handleToId_.find(m->GetConnection());
            if (it != handleToId_.end() && onMessage_) {
                onMessage_(it->second,
                           std::span<const std::byte>(
                               reinterpret_cast<const std::byte*>(m->GetData()),
                               m->GetSize()));
            }
            m->Release();
        }
    }
}

// --- callback path ---

void GnsTransport::statusChangedThunk(void* info) {
    auto* p = static_cast<SteamNetConnectionStatusChangedCallback_t*>(info);
    GnsTransport* self = unpackThisPointer(p->m_info.m_nUserData);
    if (self) {
        self->handleStatusChanged(p->m_hConn, p->m_info.m_eState);
    }
}

void GnsTransport::handleStatusChanged(HSteamNetConnection h, int newState) {
    switch (newState) {
        case k_ESteamNetworkingConnectionState_Connecting: {
            // Server side: a new client wants in. Accept it and bind it
            // to our poll group. Record the engine ID BEFORE the poll
            // group call so the cleanup tail can close it on failure.
            if (handleToId_.find(h) != handleToId_.end()) {
                // We initiated this connection (client side); nothing to do.
                break;
            }
            if (sockets_->AcceptConnection(h) != k_EResultOK) {
                Log::warn("GnsTransport: AcceptConnection failed");
                sockets_->CloseConnection(h, 0, nullptr, false);
                return;
            }
            const ConnectionId id = registerConnection(h);
            // Re-apply our user-data on the accepted connection — it
            // didn't go through the options path that connect() uses.
            sockets_->SetConnectionUserData(h, packThisPointer(this));
            if (!sockets_->SetConnectionPollGroup(h, pollGroup_)) {
                Log::warn("GnsTransport: SetConnectionPollGroup failed");
                sockets_->CloseConnection(h, 0, nullptr, false);
                unregisterConnection(id);
                return;
            }
            // Don't fire onOpened here yet — wait for Connected state.
            break;
        }

        case k_ESteamNetworkingConnectionState_Connected: {
            // Either client-side handshake just finished, or server-side
            // accept moved to Connected. Fire onOpened either way.
            auto it = handleToId_.find(h);
            if (it != handleToId_.end() && onOpened_) {
                onOpened_(it->second);
            }
            break;
        }

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            auto it = handleToId_.find(h);
            if (it != handleToId_.end()) {
                const ConnectionId id = it->second;
                // Close + remove our mapping FIRST, so a callback that
                // re-enters us can't see a stale id.
                sockets_->CloseConnection(h, 0, nullptr, false);
                unregisterConnection(id);
                if (onClosed_) {
                    onClosed_(id, "peer closed or problem detected");
                }
            } else {
                sockets_->CloseConnection(h, 0, nullptr, false);
            }
            break;
        }

        default:
            break;
    }
}

// --- mapping helpers ---

ConnectionId GnsTransport::registerConnection(HSteamNetConnection h) {
    const ConnectionId id = nextId_++;
    idToHandle_[id] = h;
    handleToId_[h]  = id;
    return id;
}

void GnsTransport::unregisterConnection(ConnectionId conn) {
    auto it = idToHandle_.find(conn);
    if (it == idToHandle_.end()) return;
    handleToId_.erase(it->second);
    idToHandle_.erase(it);
}

HSteamNetConnection GnsTransport::lookup(ConnectionId conn) const {
    auto it = idToHandle_.find(conn);
    return (it == idToHandle_.end()) ? k_HSteamNetConnection_Invalid : it->second;
}

} // namespace iron
```

NOTE for the implementer: `lookup` is declared in the header but only used internally; if the linker warns about an unused function, either use it (replace `auto it = idToHandle_.find(conn)` patterns with it) or remove the declaration. Lean toward keeping it as a clear named helper.

- [ ] **Step 3: Wire CMake**

Edit `engine/CMakeLists.txt`. Add `net/backends/gns/GnsTransport.cpp` right after the mock one:
```cmake
  net/NetTransport.cpp
  net/backends/mock/MockTransport.cpp
  net/backends/gns/GnsTransport.cpp
```

In the same file, change the `target_link_libraries(ironcore ...)` line. Currently:
```cmake
target_link_libraries(ironcore PUBLIC glfw glad_gl_core_33 stb_image)
```

Change to:
```cmake
target_link_libraries(ironcore PUBLIC
  glfw
  glad_gl_core_33
  stb_image
  GameNetworkingSockets::GameNetworkingSockets)
```

PUBLIC link so games linking `ironcore` get GNS transitively.

- [ ] **Step 4: Build ironcore + all tests to confirm nothing broke**

```powershell
cmake --build build
```

Use timeout: 600000. Expected: clean build, all 17 tests still compile (16 existing + 1 new from Task 2).

- [ ] **Step 5: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 17`.

- [ ] **Step 6: Commit**

```powershell
git add engine/net/backends/gns engine/CMakeLists.txt
git commit -m "Engine: GnsTransport (NetTransport over GameNetworkingSockets); link GNS PUBLIC into ironcore" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 4: Rewrite ping-pong on the wrapper

**Files:**
- Modify: `games/04-net-pingpong/CMakeLists.txt`
- Modify: `games/04-net-pingpong/main.cpp` (full rewrite)

- [ ] **Step 1: Trim the CMakeLists**

Edit `games/04-net-pingpong/CMakeLists.txt`. Replace the whole file with:

```cmake
add_executable(net-pingpong main.cpp)
target_link_libraries(net-pingpong PRIVATE ironcore)
```

GNS is now transitive via ironcore — the direct `GameNetworkingSockets::GameNetworkingSockets` link is unnecessary (and removing it forces the ping-pong source to stay GNS-header-free).

- [ ] **Step 2: Rewrite `games/04-net-pingpong/main.cpp`**

Replace the entire file with:

```cpp
// Iron Core Engine — networking smoke test (M8.1, on iron::NetTransport).
//
// Single process, two GnsTransport endpoints on the same localhost UDP
// port. Server listens; client connects to itself; client sends PING
// reliably; server replies PONG; both messages are verified; the program
// prints "OK" and exits with code 0.
//
// This is the integration test for iron::GnsTransport — the unit test
// for the contract lives in tests/test_mock_net_transport.cpp.
//
// Failure modes (all exit code 1):
//   - GnsTransport::start fails
//   - listen/connect returns kInvalidConnection
//   - either side fails to open within 5 s
//   - PING or PONG mismatch
//   - any other 5 s wall-clock timeout

#include "core/Log.h"
#include "net/NetTransport.h"
#include "net/backends/gns/GnsTransport.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::span<const std::byte> asBytes(std::string_view sv) {
    return {reinterpret_cast<const std::byte*>(sv.data()), sv.size()};
}

std::string_view asStr(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

}  // namespace

int main() {
    iron::GnsTransport server;
    iron::GnsTransport client;  // NOTE: see "Important" below

    bool done = false;
    bool failed = false;
    iron::ConnectionId clientConn = iron::kInvalidConnection;

    auto failWith = [&](const char* reason) {
        std::fprintf(stderr, "net-pingpong: %s\n", reason);
        failed = true;
    };

    server.setOnMessage([&](iron::ConnectionId c, std::span<const std::byte> b) {
        const auto msg = asStr(b);
        if (msg != "PING") { failWith("server received non-PING"); return; }
        if (!server.send(c, asBytes("PONG"), iron::SendReliability::Reliable)) {
            failWith("server send PONG failed");
        }
    });
    server.setOnConnectionClosed([&](iron::ConnectionId, const std::string&) {
        if (!done) failWith("server connection closed before exchange completed");
    });

    client.setOnConnectionOpened([&](iron::ConnectionId c) {
        if (c != clientConn) return;
        if (!client.send(c, asBytes("PING"), iron::SendReliability::Reliable)) {
            failWith("client send PING failed");
        }
    });
    client.setOnMessage([&](iron::ConnectionId, std::span<const std::byte> b) {
        if (asStr(b) == "PONG") done = true;
        else failWith("client received non-PONG");
    });
    client.setOnConnectionClosed([&](iron::ConnectionId, const std::string&) {
        if (!done) failWith("client connection closed before exchange completed");
    });

    if (!server.start() || !client.start()) { failWith("transport start failed"); return 1; }

    const iron::NetAddress addr{0x7F000001, 27015};
    if (!server.listen(addr)) { failWith("server.listen failed"); return 1; }
    clientConn = client.connect(addr);
    if (clientConn == iron::kInvalidConnection) { failWith("client.connect failed"); return 1; }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done && !failed && std::chrono::steady_clock::now() < deadline) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done && !failed) failWith("timeout waiting for ping-pong exchange");

    server.stop();
    client.stop();

    if (done && !failed) {
        std::printf("OK\n");
        return 0;
    }
    return 1;
}
```

**Important constraint** (read before building): The spec says one
GnsTransport per process — GameNetworkingSockets_Init/Kill are
process-global. The ping-pong exe currently uses two GnsTransport
instances (server + client). Test this combination locally first: if
GNS rejects the second `Init`, the smoke test will print
"transport start failed" for the client. If that happens, fall back to
a single GnsTransport that both listens AND connects (GNS supports
this; the connection from client.connect on the same interface will
loopback to its own listen socket). Adjust the source to use one
`iron::GnsTransport transport;` and both `transport.listen(addr)` and
`transport.connect(addr)` on it.

The single-transport version still proves the wrapper contract — both
events flow through one `transport.poll()` call.

- [ ] **Step 3: Build the exe**

```powershell
cmake --build build --target net-pingpong
```

Use timeout: 180000. Expected: clean build.

- [ ] **Step 4: Run the smoke test**

```powershell
./build/games/04-net-pingpong/Debug/net-pingpong.exe; "exit code: $LASTEXITCODE"
```

Expected:
```
OK
exit code: 0
```

If you see `transport start failed`, switch to the single-transport variant per the "Important" note in Step 2, rebuild, rerun.

- [ ] **Step 5: Run all tests to confirm nothing else broke**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 17`.

- [ ] **Step 6: Commit**

```powershell
git add games/04-net-pingpong
git commit -m "Networking: rewrite ping-pong on iron::NetTransport wrapper" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 5: Engine networking docs

**Files:**
- Create: `docs/engine/networking.md`

- [ ] **Step 1: Create the doc**

```markdown
# Engine networking

The engine exposes a transport-agnostic networking interface,
`iron::NetTransport`, with two concrete implementations:

- `iron::GnsTransport` — production. Wraps Valve's
  [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets):
  UDP with reliable + unreliable channels, encryption, congestion control,
  fragmentation. Works standalone (no Steam needed).
- `iron::MockTransport` — test-only. Pairs in-process via a static
  registry; no real sockets, deterministic, instant. Useful for unit
  tests and (eventually) offline single-player simulation of code paths
  written against the transport.

## Layout

```
engine/net/
├── NetTransport.h         # abstract base + NetAddress, ConnectionId, SendReliability
├── backends/
│   ├── gns/GnsTransport   # production
│   └── mock/MockTransport # tests
```

Game code includes `net/NetTransport.h` and the concrete backend header it
needs (`net/backends/gns/GnsTransport.h` for production, `net/backends/mock/MockTransport.h`
for tests). It never includes any `<steam/...>` headers — those live
entirely inside `GnsTransport.cpp`.

## Driving model

1. Construct the transport.
2. Set observer callbacks via the `setOn...` methods.
3. `start()` it.
4. `listen(addr)` and/or `connect(addr)`.
5. Once per game-loop tick: `transport.poll()`. The transport fires the
   observer callbacks for state changes and inbound messages on the
   calling thread.
6. `send(conn, bytes, reliability)` to push bytes.
7. `close(conn)` to drop a connection (peer's `onConnectionClosed` fires;
   local side does NOT fire — caller already knows).
8. `stop()` on shutdown.

## What this layer does NOT do (yet)

- No protocol / typed messages — you send raw bytes.
- No replication / "network variables".
- No tick scheduling, snapshot interpolation, prediction, rollback.
- No Steam Datagram Relay, no NAT traversal.
- No IPv6 or hostname resolution — IPv4 only.

These land in later milestones once game integration grounds the design.

## The smoke test

`games/04-net-pingpong/main.cpp` is a single-process exe that uses
GnsTransport to open a listener and a client on `127.0.0.1:27015`,
exchange PING + PONG reliably, and exit with code 0 (or 1 + a stderr
diagnostic on any failure). CI runs it as part of every build — it's
our end-to-end check that the wrapper works against real sockets.

The unit test for the contract — `tests/test_mock_net_transport.cpp` —
runs the same contract against MockTransport (no real sockets) on every
CTest invocation.
```

- [ ] **Step 2: Commit**

```powershell
git add docs/engine/networking.md
git commit -m "Docs: engine networking overview (M8.1)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 6: Code review pass + PR

**Files:** none modified — read-only review (plus any fixes the review surfaces)

- [ ] **Step 1: Show the diff range**

```powershell
git log --oneline main..HEAD
git diff main --stat
```

Expected: 5 commits (Tasks 1–5), the files listed in this plan's "File Structure" section.

- [ ] **Step 2: Dispatch a code-quality review agent**

Dispatch `feature-dev:code-reviewer` (or `general-purpose`) with this prompt:

> Review the M8.1 net-transport-wrapper changes (`git diff main`) in the Iron Core Engine. The milestone adds `iron::NetTransport` abstract base + `iron::GnsTransport` (GameNetworkingSockets production impl) + `iron::MockTransport` (in-process loopback for tests), rewrites `games/04-net-pingpong/main.cpp` on top of the wrapper, and adds a unit-test suite against MockTransport. Spec: `docs/superpowers/specs/2026-05-24-net-transport-wrapper-design.md`.
>
> Focus on:
> 1. **NetTransport interface correctness** — is the contract clearly specified? Are the lifecycle/ownership rules unambiguous? Anything a future second backend would have to fight?
> 2. **GnsTransport callback safety** — the `m_nUserData` round-trip for `this` recovery, accept-then-set-poll-group failure paths, double-close/dangling-handle hygiene. Compare to the PR #18 review findings (M8.0 had a handle leak in the Connecting branch and a dangling handle after close-in-callback — confirm both are properly resolved here, not just superficially).
> 3. **MockTransport correctness** — registry lifetime, in-flight events during `stop()`, what happens if `poll()` is called from inside a callback.
> 4. **Test coverage** — do the 6 cases actually exercise the contract, or do they short-circuit?
> 5. **CMake** — is GNS now exposed publicly via ironcore correctly? Did ping-pong's link line shrink as planned?
> 6. **Header hygiene** — does any file in `games/` accidentally include `<steam/...>`?
>
> Skip style nits. Cap at 10 findings. Under 400 words. End with **APPROVED** or **NEEDS_FIXES**.

- [ ] **Step 3: Address findings**

Apply fixes. Push back on cosmetic suggestions; only block on real correctness issues.

- [ ] **Step 4: Build + smoke test + full ctest one more time**

```powershell
cmake --build build
./build/games/04-net-pingpong/Debug/net-pingpong.exe
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, `OK` from net-pingpong, `100% tests passed, 0 tests failed out of 17`.

- [ ] **Step 5: Commit review fixes (skip if no fixes needed)**

```powershell
git add -A
git commit -m "Networking: address M8.1 code-review findings" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

- [ ] **Step 6: Push and open the PR**

```powershell
git push -u origin feat/net-transport-wrapper
gh pr create --title "Milestone 8.1: Engine networking wrapper (iron::NetTransport)" --body "$(cat <<'EOF'
## Summary

Adds `iron::NetTransport` abstract base + two implementations under `engine/net/`. Game code now holds a `NetTransport*` and never includes `<steam/...>` headers.

- `iron::NetTransport` (engine/net/NetTransport.h) — abstract base + `NetAddress`, `ConnectionId`, `SendReliability`. Driving model documented inline.
- `iron::GnsTransport` (engine/net/backends/gns) — wraps GameNetworkingSockets. Recovers `this` from the C-style status callback via the per-connection user-data slot — no more file-scope `g_state` pointer. The two HIGH bugs flagged in PR #18 review (handle leak on `Connecting` failure; dangling handle after close-in-callback) are fixed at the source.
- `iron::MockTransport` (engine/net/backends/mock) — in-process loopback for unit tests. Paired instances discover each other via a static registry keyed on listen address.
- `games/04-net-pingpong/main.cpp` rewritten on the wrapper (~80 lines, down from ~240). CI integration test unchanged.
- `tests/test_mock_net_transport.cpp` — 6 contract cases.
- `ironcore` now PUBLIC-links GNS (every game inherits transitively).
- `docs/engine/networking.md` — short intro + driving-model bullets.

## Test plan

- [x] `test_mock_net_transport` passes (6 contract cases)
- [x] All 17 unit tests pass
- [x] `net-pingpong.exe` prints `OK`, exits 0
- [x] No `games/*/*.cpp` includes `<steam/...>` (verified with grep)

## Out of scope (M8.2+)

- Protocol / typed messages / schema / versioning
- Replication, "network variables"
- Tick / snapshot / prediction / rollback
- Steam Datagram Relay, Steam auth
- IPv6, hostname resolution
- Cross-machine testing
- Game integration (multiplayer Strandbound is several milestones out)

EOF
)"
```

Return the PR URL when done.

---

## Self-review (run after writing the plan, before handoff)

Already done inline:

- **Spec coverage:**
  - `iron::NetTransport` abstract base + types → Task 1
  - `iron::GnsTransport` implementation including user-data callback dispatch → Task 3
  - `iron::MockTransport` in-process loopback → Task 2
  - ping-pong rewrite → Task 4
  - 6 contract test cases → Task 2 (Step 1)
  - `ironcore` links GNS PUBLIC → Task 3 (Step 3)
  - ping-pong drops direct GNS link → Task 4 (Step 1)
  - docs/engine/networking.md → Task 5
  - Code review pass per `always-code-review-changes` memory → Task 6
- **Non-goals from the spec respected:** No protocol layer, no replication, no tick model, no Steam Datagram, no IPv6, no game integration, no threading change. Plan doesn't introduce any of these.
- **Placeholder scan:** no TBD/TODO/"add error handling" patterns. Every step has the actual code.
- **Type consistency:**
  - `ConnectionId`, `NetAddress`, `SendReliability`, `OnConnectionOpenedFn` etc. consistent across header, both impls, tests, and ping-pong.
  - `kInvalidConnection` used everywhere.
  - Methods on `MockTransport` and `GnsTransport` match the override signatures from `NetTransport`.
- **One known acknowledgement (not a placeholder):** Task 4 Step 2 includes an "Important" note about the possibility that two `GnsTransport` instances in one process fight over `GameNetworkingSockets_Init`. The plan explicitly tells the implementer what to fall back to. That's intentional — local validation will resolve it, and the fallback is well-specified.
