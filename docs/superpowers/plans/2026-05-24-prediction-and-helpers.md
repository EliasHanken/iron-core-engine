# Prediction Stack + Helpers (M8.5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `iron::PeerManager` (owns Hello + peer lifecycle + conn↔peerId; reserves tag=1 for `peer::HelloMsg`) and `iron::PredictionEngine<TInput, TState>` (client-side input history + reconciliation); refactor net-cubes to use PeerManager (cleanup); refactor net-tag to use PeerManager + PredictionEngine + server-authoritative position; document the snapshot pattern.

**Architecture:** Two new engine helpers, both header-only template (`PredictionEngine`) or thin .cpp (`PeerManager`). `PeerManager` installs its own `setOnMessage` lambda... wait, actually `MessageRegistry` already owns that; `PeerManager` installs `setOnConnectionOpened`/`setOnConnectionClosed` on the transport and registers a `peer::HelloMsg` handler on the registry. Game registers its own handlers at tag≥2. Net-tag's wire format changes (delete HelloMsg, add PlayerInputMsg + AuthorityPositionMsg, renumber TagSwap+). Net-cubes refactor is purely a code cleanup with no wire-format or behavior change.

**Tech Stack:** C++23 templates, MockTransport for tests, custom CTest harness.

**Spec:** `docs/superpowers/specs/2026-05-24-prediction-and-helpers-design.md`

---

## File Structure

**New files:**
- `engine/net/PeerMessages.h` — `peer::HelloMsg` (kTag=1 reserved)
- `engine/net/PeerManager.h` + `.cpp` — owns lifecycle, Hello handling, conn↔peerId maps
- `engine/net/PredictionEngine.h` — header-only template
- `tests/test_peer_manager.cpp` — 5 cases against paired MockTransports
- `tests/test_prediction_engine.cpp` — 5 cases (apply, match, mismatch, stale, reset)

**Modified files:**
- `engine/CMakeLists.txt` — add `net/PeerManager.cpp` source
- `tests/CMakeLists.txt` — register two new tests
- `games/05-net-cubes/Messages.h` — delete `HelloMsg` (PeerManager owns it)
- `games/05-net-cubes/main.cpp` — adopt PeerManager; ~80 lines removed
- `games/06-net-tag/Messages.h` — delete `HelloMsg`; add `PlayerInputMsg` + `AuthorityPositionMsg`; renumber TagSwap+
- `games/06-net-tag/main.cpp` — adopt PeerManager + PredictionEngine + server-authoritative position
- `docs/engine/networking.md` — PeerManager, PredictionEngine, Snapshot pattern sections

---

## Task 0: Branch setup

**Files:** none (git state only)

`main` is protected — work goes on a feature branch.

- [ ] **Step 1: Create and switch to the feature branch**

```powershell
git checkout -b feat/prediction-and-helpers
git status
```

Expected: `On branch feat/prediction-and-helpers`. Modified docs files (CRLF/LF noise) harmless.

---

## Task 1: `iron::PeerManager` (TDD)

**Files:**
- Create: `engine/net/PeerMessages.h`
- Create: `engine/net/PeerManager.h`
- Create: `engine/net/PeerManager.cpp`
- Create: `tests/test_peer_manager.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

PeerManager owns the boilerplate every networked game writes today. Reserves message tag=1.

- [ ] **Step 1: Write the failing test `tests/test_peer_manager.cpp`**

```cpp
#include "test_framework.h"
#include "net/MessageRegistry.h"
#include "net/NetTransport.h"
#include "net/PeerManager.h"
#include "net/backends/mock/MockTransport.h"
#include "core/NetArgs.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace iron;

namespace {

constexpr std::uint32_t kGameA = 0xAABBCCDDu;
constexpr std::uint32_t kGameB = 0x11223344u;
constexpr NetAddress kAddr{0x7F000001u, 5555};

// Game-defined message at tag=2 (since tag=1 is reserved by PeerManager).
struct PingMsg { static constexpr std::uint8_t kTag = 2; std::uint32_t value; };

NetArgs hostArgs() { NetArgs a; a.addr = kAddr; a.wantsConnect = false; return a; }
NetArgs clientArgs() { NetArgs a; a.addr = kAddr; a.wantsConnect = true; return a; }

}  // namespace

int main() {
    // Case 1: paired connect → both sides fire onPeerJoined.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGameA);
        PeerManager cli(cliT, cliR, kGameA);

        std::vector<std::uint32_t> srvJoined, cliJoined;
        srv.setOnPeerJoined([&](std::uint32_t pid) { srvJoined.push_back(pid); });
        cli.setOnPeerJoined([&](std::uint32_t pid) { cliJoined.push_back(pid); });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));

        // host fires onPeerJoined(0) for self at start()
        CHECK(srvJoined.size() == 1);
        CHECK(srvJoined[0] == 0);

        // pump: client connects → server fires onPeerJoined(1); server sends
        // Hello → client fires onPeerJoined(myPeerId=1) then onPeerJoined(0=host).
        srv.poll(); cli.poll(); srv.poll(); cli.poll();

        CHECK(srv.isHost());
        CHECK(!cli.isHost());
        CHECK(srv.myPeerId() == 0);
        CHECK(cli.myPeerId() == 1);

        CHECK(srvJoined.size() == 2);
        CHECK(srvJoined[1] == 1);
        CHECK(cliJoined.size() == 2);
        // Client fires onPeerJoined for self and host (order: self, then 0).
        CHECK(cliJoined[0] == 1);
        CHECK(cliJoined[1] == 0);
    }

    // Case 2: wrong gameId → client logs error and closes; host sees disconnect.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGameA);
        PeerManager cli(cliT, cliR, kGameB);  // different gameId

        std::vector<std::uint32_t> srvLeft;
        srv.setOnPeerLeft([&](std::uint32_t pid) { srvLeft.push_back(pid); });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));

        // Pump enough to: server accept → server sends Hello → client receives
        // → client rejects + closes → server sees disconnect.
        for (int i = 0; i < 6; ++i) { srv.poll(); cli.poll(); }

        // Client never got an identity.
        CHECK(cli.myPeerId() == 0);
        CHECK(!cli.hasIdentity());

        // Server eventually sees the client's connection close.
        CHECK(srvLeft.size() == 1);
        CHECK(srvLeft[0] == 1);
    }

    // Case 3: send() / broadcastToAll() routing.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGameA);
        PeerManager cli(cliT, cliR, kGameA);

        std::vector<std::uint32_t> srvReceived;
        srvR.registerHandler<PingMsg>([&](ConnectionId, const PingMsg& m) {
            srvReceived.push_back(m.value);
        });
        std::vector<std::uint32_t> cliReceived;
        cliR.registerHandler<PingMsg>([&](ConnectionId, const PingMsg& m) {
            cliReceived.push_back(m.value);
        });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));
        for (int i = 0; i < 4; ++i) { srv.poll(); cli.poll(); }

        // Client sends to host (peerId 0) using PeerManager.send.
        CHECK(cli.send<PingMsg>(0, PingMsg{42}, SendReliability::Reliable));
        srv.poll();
        CHECK(srvReceived.size() == 1);
        CHECK(srvReceived[0] == 42);

        // Host broadcasts to all (one client, peerId 1).
        srv.broadcastToAll<PingMsg>(PingMsg{99}, SendReliability::Reliable);
        cli.poll();
        CHECK(cliReceived.size() == 1);
        CHECK(cliReceived[0] == 99);
    }

    // Case 4: disconnect → onPeerLeft fires.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGameA);
        PeerManager cli(cliT, cliR, kGameA);

        std::vector<std::uint32_t> srvLeft, cliLeft;
        srv.setOnPeerLeft([&](std::uint32_t pid) { srvLeft.push_back(pid); });
        cli.setOnPeerLeft([&](std::uint32_t pid) { cliLeft.push_back(pid); });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));
        for (int i = 0; i < 4; ++i) { srv.poll(); cli.poll(); }

        // Client stops → server should see onPeerLeft(1).
        cli.stop();
        for (int i = 0; i < 4; ++i) { srv.poll(); }
        CHECK(srvLeft.size() == 1);
        CHECK(srvLeft[0] == 1);
    }

    // Case 5: peerIds / connectionFor / peerIdFor accessors.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGameA);
        PeerManager cli(cliT, cliR, kGameA);

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));
        for (int i = 0; i < 4; ++i) { srv.poll(); cli.poll(); }

        // From host's perspective:
        const auto peers = srv.peerIds();
        CHECK(peers.size() == 1);
        CHECK(peers[0] == 1);

        const auto conn1 = srv.connectionFor(1);
        CHECK(conn1 != kInvalidConnection);

        const auto pid = srv.peerIdFor(conn1);
        CHECK(pid.has_value());
        CHECK(*pid == 1);

        // Unknown peerId → invalid connection
        CHECK(srv.connectionFor(99) == kInvalidConnection);
        // Unknown ConnectionId → nullopt
        CHECK(!srv.peerIdFor(99).has_value());
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Verify build fails (header missing)**

```powershell
cmake --build build --target test_peer_manager
```

Expected: `'net/PeerManager.h': No such file or directory`.

- [ ] **Step 3: Create `engine/net/PeerMessages.h`**

```cpp
#pragma once

#include <cstdint>

namespace iron::peer {

// PeerManager-owned Hello message. Reserved at tag=1 across the whole
// engine — every game's MessageRegistry tags must start at 2. Host
// sends this to each new client on connect; client validates gameId
// and learns its assigned peerId.
struct HelloMsg {
    static constexpr std::uint8_t kTag = 1;
    std::uint32_t gameId;
    std::uint32_t peerId;
};

}  // namespace iron::peer
```

- [ ] **Step 4: Create `engine/net/PeerManager.h`**

```cpp
#pragma once

#include "core/Log.h"
#include "core/NetArgs.h"
#include "net/MessageRegistry.h"
#include "net/NetTransport.h"
#include "net/PeerMessages.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace iron {

// Owns peer lifecycle, Hello handshake, gameId validation, and the
// conn↔peerId map. Reserves message tag=1 for peer::HelloMsg — every
// game's MessageRegistry tags must start at 2.
//
// Constructor installs the Hello handler on the registry and the
// connection-opened/closed callbacks on the transport. Game MUST NOT
// register its own handler for peer::HelloMsg::kTag (1) nor set its
// own setOnConnectionOpened/setOnConnectionClosed callbacks AFTER
// constructing a PeerManager.
//
// Lifecycle hooks:
//   onPeerJoined(peerId) — fires for every peer the local node should
//                          know about, including peerId 0 (the host)
//                          if we are NOT the host. Host fires
//                          onPeerJoined(0) for itself inside start().
//   onPeerLeft(peerId)   — fires when a known peer disconnects.
//
// Game state init must subscribe to onPeerJoined/Left BEFORE calling
// start(), because host's start() synchronously fires onPeerJoined(0)
// for itself.
class PeerManager {
public:
    using PeerJoinedFn = std::function<void(std::uint32_t)>;
    using PeerLeftFn   = std::function<void(std::uint32_t)>;

    PeerManager(NetTransport& transport, MessageRegistry& registry,
                std::uint32_t gameId);
    ~PeerManager();

    PeerManager(const PeerManager&) = delete;
    PeerManager& operator=(const PeerManager&) = delete;

    // Opens the transport per NetArgs. Returns false on failure
    // (transport.start() refused, neither listen nor connect worked).
    bool start(const NetArgs& args);
    void stop();

    bool isHost() const { return isHost_; }
    std::uint32_t myPeerId() const { return myPeerId_; }
    bool hasIdentity() const { return isHost_ || myPeerId_ != 0; }

    std::vector<std::uint32_t> peerIds() const;
    ConnectionId connectionFor(std::uint32_t peerId) const;
    std::optional<std::uint32_t> peerIdFor(ConnectionId conn) const;

    void setOnPeerJoined(PeerJoinedFn fn) { onJoined_ = std::move(fn); }
    void setOnPeerLeft(PeerLeftFn fn)     { onLeft_ = std::move(fn); }

    void poll();

    // Send to one peer by peerId. Returns false if peerId is unknown.
    template <typename Msg>
    bool send(std::uint32_t peerId, const Msg& msg, SendReliability r) {
        const ConnectionId c = connectionFor(peerId);
        if (c == kInvalidConnection) return false;
        return registry_.send<Msg>(c, msg, r);
    }

    // Host-only: broadcast to every connected client. No-op if not host.
    template <typename Msg>
    void broadcastToAll(const Msg& msg, SendReliability r) {
        if (!isHost_) return;
        for (const auto& [c, _] : connToPeerId_) {
            registry_.send<Msg>(c, msg, r);
        }
    }

private:
    void handleHello(ConnectionId c, const peer::HelloMsg& msg);
    void handleConnectionOpened(ConnectionId c);
    void handleConnectionClosed(ConnectionId c, const std::string& reason);

    NetTransport& transport_;
    MessageRegistry& registry_;
    std::uint32_t gameId_;
    bool started_ = false;
    bool isHost_ = false;
    std::uint32_t myPeerId_ = 0;
    std::uint32_t nextPeerId_ = 1;
    ConnectionId hostConn_ = kInvalidConnection;
    std::unordered_map<ConnectionId, std::uint32_t> connToPeerId_;
    std::unordered_map<std::uint32_t, ConnectionId> peerIdToConn_;
    PeerJoinedFn onJoined_;
    PeerLeftFn   onLeft_;
};

}  // namespace iron
```

- [ ] **Step 5: Create `engine/net/PeerManager.cpp`**

```cpp
#include "net/PeerManager.h"

#include <utility>

namespace iron {

PeerManager::PeerManager(NetTransport& transport, MessageRegistry& registry,
                         std::uint32_t gameId)
    : transport_(transport), registry_(registry), gameId_(gameId) {
    registry_.registerHandler<peer::HelloMsg>(
        [this](ConnectionId c, const peer::HelloMsg& msg) {
            this->handleHello(c, msg);
        });
    transport_.setOnConnectionOpened(
        [this](ConnectionId c) { this->handleConnectionOpened(c); });
    transport_.setOnConnectionClosed(
        [this](ConnectionId c, const std::string& reason) {
            this->handleConnectionClosed(c, reason);
        });
}

PeerManager::~PeerManager() {
    stop();
    // Drop our callbacks from the transport so it won't dispatch into
    // our dangling state. We can't really unhook our HelloMsg handler
    // from the registry — it owns the lambda — but the registry should
    // outlive the PeerManager (caller's responsibility).
    transport_.setOnConnectionOpened(NetTransport::OnConnectionOpenedFn{});
    transport_.setOnConnectionClosed(NetTransport::OnConnectionClosedFn{});
}

bool PeerManager::start(const NetArgs& args) {
    if (started_) return true;

    if (!transport_.start()) {
        Log::error("PeerManager: transport.start failed");
        return false;
    }

    if (args.wantsConnect) {
        hostConn_ = transport_.connect(args.addr);
        if (hostConn_ == kInvalidConnection) {
            Log::error("PeerManager: connect failed");
            transport_.stop();
            return false;
        }
        isHost_ = false;
    } else {
        isHost_ = transport_.listen(args.addr);
        if (!isHost_) {
            // Fallback: try connect (single-machine "double click"
            // experience).
            hostConn_ = transport_.connect(args.addr);
            if (hostConn_ == kInvalidConnection) {
                Log::error("PeerManager: neither listen nor connect succeeded");
                transport_.stop();
                return false;
            }
        }
    }

    started_ = true;

    // Host fires onPeerJoined(0) for self synchronously so game's
    // per-peer init path is uniform.
    if (isHost_ && onJoined_) {
        onJoined_(0);
    }

    return true;
}

void PeerManager::stop() {
    if (!started_) return;
    transport_.stop();
    connToPeerId_.clear();
    peerIdToConn_.clear();
    isHost_ = false;
    myPeerId_ = 0;
    hostConn_ = kInvalidConnection;
    nextPeerId_ = 1;
    started_ = false;
}

std::vector<std::uint32_t> PeerManager::peerIds() const {
    std::vector<std::uint32_t> out;
    out.reserve(peerIdToConn_.size());
    for (const auto& [pid, _] : peerIdToConn_) out.push_back(pid);
    return out;
}

ConnectionId PeerManager::connectionFor(std::uint32_t peerId) const {
    auto it = peerIdToConn_.find(peerId);
    return it == peerIdToConn_.end() ? kInvalidConnection : it->second;
}

std::optional<std::uint32_t> PeerManager::peerIdFor(ConnectionId conn) const {
    auto it = connToPeerId_.find(conn);
    if (it == connToPeerId_.end()) return std::nullopt;
    return it->second;
}

void PeerManager::poll() {
    transport_.poll();
}

void PeerManager::handleHello(ConnectionId c, const peer::HelloMsg& msg) {
    if (isHost_) {
        // Host should never receive Hello — clients receive it from us.
        Log::warn("PeerManager: host received Hello — ignoring");
        return;
    }
    if (msg.gameId != gameId_) {
        Log::error(
            "PeerManager: connected to wrong game (gameId=0x%08X, expected 0x%08X) — disconnecting",
            msg.gameId, gameId_);
        transport_.close(c);
        return;
    }
    if (myPeerId_ != 0) {
        Log::warn("PeerManager: received duplicate Hello — ignoring");
        return;
    }
    myPeerId_ = msg.peerId;
    // Client's "peerId 0" is the host on the other end of hostConn_.
    connToPeerId_[c] = 0;
    peerIdToConn_[0] = c;
    if (onJoined_) {
        onJoined_(myPeerId_);  // self
        onJoined_(0);          // host
    }
}

void PeerManager::handleConnectionOpened(ConnectionId c) {
    if (isHost_) {
        const std::uint32_t assigned = nextPeerId_++;
        connToPeerId_[c] = assigned;
        peerIdToConn_[assigned] = c;
        registry_.send<peer::HelloMsg>(
            c, peer::HelloMsg{gameId_, assigned},
            SendReliability::Reliable);
        if (onJoined_) onJoined_(assigned);
    }
    // Client: do nothing until HelloMsg arrives.
}

void PeerManager::handleConnectionClosed(ConnectionId c,
                                          const std::string& reason) {
    (void)reason;
    auto it = connToPeerId_.find(c);
    if (it == connToPeerId_.end()) return;
    const std::uint32_t pid = it->second;
    connToPeerId_.erase(it);
    peerIdToConn_.erase(pid);
    if (onLeft_) onLeft_(pid);
}

}  // namespace iron
```

- [ ] **Step 6: Wire CMake**

Edit `engine/CMakeLists.txt`. Add `net/PeerManager.cpp` to the `ironcore` source list, near the other `net/` entries:

```cmake
  net/MessageRegistry.cpp
  net/PeerManager.cpp
```

Edit `tests/CMakeLists.txt`. Add after the existing `iron_add_test(test_message_registry ...)` line:

```cmake
iron_add_test(test_peer_manager test_peer_manager.cpp)
```

- [ ] **Step 7: Build + run the test**

```powershell
cmake --build build --target test_peer_manager
ctest --test-dir build -C Debug -R test_peer_manager --output-on-failure
```

Use `timeout: 180000`. Expected: `OK - all checks passed`, CTest PASS.

- [ ] **Step 8: Run all tests to confirm no regressions**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 22/22 pass (21 existing + new `test_peer_manager`).

- [ ] **Step 9: Commit**

```powershell
git add engine/net/PeerMessages.h engine/net/PeerManager.h engine/net/PeerManager.cpp tests/test_peer_manager.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "Engine: PeerManager owns Hello + peer lifecycle (tag=1 reserved)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: `iron::PredictionEngine<TInput, TState>` (TDD)

**Files:**
- Create: `engine/net/PredictionEngine.h`
- Create: `tests/test_prediction_engine.cpp`
- Modify: `tests/CMakeLists.txt`

Header-only template. No engine `.cpp` needed.

- [ ] **Step 1: Write the failing test**

```cpp
#include "test_framework.h"
#include "net/PredictionEngine.h"

using namespace iron;

int main() {
    // Trivial sim: state += input. dt ignored.
    auto sim = [](const int& s, const int& i, float) -> int { return s + i; };

    // Case 1: applyInput accumulates predicted state and history.
    {
        PredictionEngine<int, int> e{sim, 0.033f, /*initial*/0};
        const auto id1 = e.applyInput(1); CHECK(id1 == 1);
        const auto id2 = e.applyInput(2); CHECK(id2 == 2);
        const auto id3 = e.applyInput(3); CHECK(id3 == 3);
        CHECK(e.predictedState() == 6);   // 0 + 1 + 2 + 3
        CHECK(e.historySize() == 3);
    }

    // Case 2: reconcile match → drops confirmed, predicted unchanged.
    {
        PredictionEngine<int, int> e{sim, 0.033f, /*initial*/0};
        e.applyInput(1);    // predicted = 1
        e.applyInput(2);    // predicted = 3
        e.applyInput(3);    // predicted = 6
        // Server confirms input 2 with authState=3 (matches our prediction at input 2).
        e.reconcile(3, /*lastConfirmedInputId*/2);
        CHECK(e.predictedState() == 6);   // unchanged
        CHECK(e.historySize() == 1);      // only input 3 remains
    }

    // Case 3: reconcile mismatch → snap + replay.
    {
        PredictionEngine<int, int> e{sim, 0.033f, /*initial*/0};
        e.applyInput(1);    // predicted = 1
        e.applyInput(2);    // predicted = 3
        e.applyInput(3);    // predicted = 6
        // Server says authState at input 2 was 100 (we predicted 3).
        // Snap to 100, replay input 3 → 103.
        e.reconcile(100, /*lastConfirmedInputId*/2);
        CHECK(e.predictedState() == 103);
        CHECK(e.historySize() == 1);
    }

    // Case 4: stale reconcile (older than what we've already confirmed) → ignored.
    {
        PredictionEngine<int, int> e{sim, 0.033f, /*initial*/0};
        e.applyInput(1); e.applyInput(2); e.applyInput(3);  // predicted = 6
        e.reconcile(3, 2);  // confirms input 2, history is now [input 3], predicted = 6
        const int before = e.predictedState();
        const std::size_t hSize = e.historySize();
        e.reconcile(999, 1);  // stale (lastConfirmedInputId=1 < history.front().inputId=3)
        CHECK(e.predictedState() == before);
        CHECK(e.historySize() == hSize);
    }

    // Case 5: reset wipes history and sets predicted.
    {
        PredictionEngine<int, int> e{sim, 0.033f, /*initial*/0};
        e.applyInput(1); e.applyInput(2);
        CHECK(e.predictedState() == 3);
        CHECK(e.historySize() == 2);
        e.reset(50);
        CHECK(e.predictedState() == 50);
        CHECK(e.historySize() == 0);
        // After reset, next inputId is 1 again.
        const auto id = e.applyInput(7);
        CHECK(id == 1);
        CHECK(e.predictedState() == 57);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Verify build fails (header missing)**

```powershell
cmake --build build --target test_prediction_engine
```

Expected: header not found.

- [ ] **Step 3: Create `engine/net/PredictionEngine.h`**

```cpp
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <utility>

namespace iron {

// Client-side input history + reconciliation against authoritative
// server state. Game provides a deterministic simulate(state, input,
// dt) → state function. PredictionEngine handles the bookkeeping:
//
//   - applyInput: advance predicted state, record (inputId, input,
//     predicted) in history. Returns the new monotonic inputId.
//
//   - predictedState(): the latest locally-predicted state.
//
//   - reconcile(authState, lastConfirmedInputId): server has applied
//     all inputs through lastConfirmedInputId and the result is
//     authState. Drop confirmed entries from history. If our prediction
//     at lastConfirmedInputId matches authState, predicted is already
//     consistent — keep it. If it doesn't (or that input is no longer
//     in history), snap predicted to authState and replay all surviving
//     history entries against it to bring "now" back up.
//
//   - reset(state): wipe history; set predicted; restart inputIds at 1.
//
// Stale reconciles (lastConfirmedInputId older than what we've already
// reconciled past) are ignored, not retried — server eventually catches
// up with newer confirmations.
//
// Comparison uses TState::operator==. Works for deterministic
// single-platform sims where client and server run the same simulate
// on the same inputs.
template <typename TInput, typename TState>
class PredictionEngine {
public:
    using SimulateFn =
        std::function<TState(const TState&, const TInput&, float dtSec)>;

    PredictionEngine(SimulateFn simulate, float fixedDtSec,
                     TState initial = {})
        : simulate_(std::move(simulate)),
          fixedDt_(fixedDtSec),
          predicted_(std::move(initial)) {}

    std::uint32_t applyInput(const TInput& input) {
        const std::uint32_t id = nextInputId_++;
        predicted_ = simulate_(predicted_, input, fixedDt_);
        history_.push_back({id, input, predicted_});
        return id;
    }

    const TState& predictedState() const { return predicted_; }

    void reconcile(const TState& authState,
                   std::uint32_t lastConfirmedInputId) {
        if (history_.empty()) {
            // No outstanding inputs; just trust the server.
            predicted_ = authState;
            return;
        }
        if (lastConfirmedInputId < history_.front().inputId) {
            // Stale confirmation — we've already reconciled past it.
            return;
        }

        // Did our prediction at lastConfirmedInputId match?
        bool matched = false;
        auto match = std::find_if(history_.begin(), history_.end(),
            [&](const Entry& e) { return e.inputId == lastConfirmedInputId; });
        if (match != history_.end()) {
            matched = (match->predicted == authState);
        }

        // Drop confirmed entries (inputId <= lastConfirmedInputId).
        while (!history_.empty() &&
               history_.front().inputId <= lastConfirmedInputId) {
            history_.pop_front();
        }

        if (matched) return;

        // Snap + replay.
        predicted_ = authState;
        for (auto& entry : history_) {
            predicted_ = simulate_(predicted_, entry.input, fixedDt_);
            entry.predicted = predicted_;
        }
    }

    std::size_t historySize() const { return history_.size(); }

    void reset(const TState& state) {
        history_.clear();
        predicted_ = state;
        nextInputId_ = 1;
    }

private:
    struct Entry {
        std::uint32_t inputId;
        TInput input;
        TState predicted;
    };

    SimulateFn simulate_;
    float fixedDt_;
    TState predicted_;
    std::uint32_t nextInputId_ = 1;
    std::deque<Entry> history_;
};

}  // namespace iron
```

- [ ] **Step 4: Wire test in `tests/CMakeLists.txt`**

Add after the `iron_add_test(test_peer_manager ...)` line:

```cmake
iron_add_test(test_prediction_engine test_prediction_engine.cpp)
```

- [ ] **Step 5: Build + run the test**

```powershell
cmake --build build --target test_prediction_engine
ctest --test-dir build -C Debug -R test_prediction_engine --output-on-failure
```

Use `timeout: 120000`. Expected: `OK - all checks passed`.

- [ ] **Step 6: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 23/23 pass.

- [ ] **Step 7: Commit**

```powershell
git add engine/net/PredictionEngine.h tests/test_prediction_engine.cpp tests/CMakeLists.txt
git commit -m "Engine: PredictionEngine<TInput, TState> for client-side prediction" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: Refactor `games/05-net-cubes` onto PeerManager

**Files:**
- Modify: `games/05-net-cubes/Messages.h` — delete HelloMsg
- Modify: `games/05-net-cubes/main.cpp` — adopt PeerManager (~80 lines removed)

Cleanup only. No wire-format change for PositionMsg (stays tag=2). No behavior change.

- [ ] **Step 1: Update `games/05-net-cubes/Messages.h`**

Use Write. Replace the entire file with:

```cpp
#pragma once

#include <cstdint>

namespace iron::netcubes {

// 4-byte ASCII game identifier passed to iron::PeerManager.
// 'n', 'E', 't', 'B' → 0x6E45'7442
constexpr std::uint32_t kGameId = 0x6E457442u;

// PositionMsg keeps tag=2 (tag=1 is reserved by iron::peer::HelloMsg
// which PeerManager owns).
struct PositionMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t peerId;
    float x, y, z;
};

}  // namespace iron::netcubes
```

(`HelloMsg` is gone — PeerManager owns it.)

- [ ] **Step 2: Update `games/05-net-cubes/main.cpp` — includes**

Use Edit. After the existing `#include "net/MessageRegistry.h"` line, add:
```cpp
#include "net/PeerManager.h"
```

- [ ] **Step 3: Remove the old peer-state declarations**

Use Edit. Find this block (around line 390-400):

```cpp
    bool isHost = false;
    iron::ConnectionId hostConn = iron::kInvalidConnection;
    std::uint32_t myPeerId = 0;       // host is always 0; client starts at 0 until Hello
    std::unordered_map<iron::ConnectionId, std::uint32_t> connToPeerId;
    std::uint32_t nextPeerId = 1;
```

DELETE it.

- [ ] **Step 4: Replace transport+registry+connection-callback setup with PeerManager**

Find this block (the `transport.setOnConnectionOpened`, `transport.setOnConnectionClosed`, and the entire `registerHandler<iron::netcubes::HelloMsg>` lambda):

```cpp
    transport.setOnConnectionOpened([&](iron::ConnectionId c) {
        if (isHost) { ... registry.send<HelloMsg>(...) ... }
    });

    transport.setOnConnectionClosed([&](iron::ConnectionId c, const std::string& reason) {
        ...
    });

    registry.registerHandler<iron::netcubes::HelloMsg>(
        [&](iron::ConnectionId, const iron::netcubes::HelloMsg& msg) {
            ...
        });
```

(Several blocks.) DELETE all three. Replace with a single `PeerManager` declaration AFTER `iron::MessageRegistry registry(&transport);`:

```cpp
    iron::PeerManager peers(transport, registry, iron::netcubes::kGameId);

    peers.setOnPeerJoined([&](std::uint32_t pid) {
        // Local cube-state init is implicit (remoteHistories uses operator[]).
        // No per-peer init needed for cubes; the position-msg flow creates
        // remoteHistories entries on demand.
        (void)pid;
    });
    peers.setOnPeerLeft([&](std::uint32_t pid) {
        remoteHistories.erase(pid);
        if (!peers.isHost()) {
            // The host left — exit cleanly.
            glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
        }
    });
```

- [ ] **Step 5: Replace `transport.start()` + listen/connect block with `peers.start(args)`**

Find this block:

```cpp
    if (!transport.start()) {
        iron::Log::error("net-cubes: GnsTransport.start failed");
        return 1;
    }

    if (wantsConnect) {
        hostConn = transport.connect(addr);
        ... etc ...
    } else {
        isHost = transport.listen(addr);
        ... etc ...
    }
```

Replace with:

```cpp
    if (!peers.start(netArgs)) {
        iron::Log::error("net-cubes: PeerManager.start failed");
        return 1;
    }
```

(The `netArgs` variable from earlier — `const iron::NetArgs netArgs = iron::parseNetArgs(argc, argv);` — is reused. Remove the now-unused `const iron::NetAddress addr = netArgs.addr;` + `const bool wantsConnect = netArgs.wantsConnect;` intermediate locals if present.)

- [ ] **Step 6: Update the PositionMsg handler to use peers.peerIdFor / peers.isHost**

Find the existing `registerHandler<iron::netcubes::PositionMsg>` body. The validation logic references `connToPeerId` — update to use `peers.peerIdFor(c)`:

```cpp
    registry.registerHandler<iron::netcubes::PositionMsg>(
        [&](iron::ConnectionId c, const iron::netcubes::PositionMsg& msg) {
            if (peers.isHost()) {
                auto pid = peers.peerIdFor(c);
                if (!pid || *pid != msg.peerId) {
                    iron::Log::warn("net-cubes: dropping PositionMsg with mismatched peerId");
                    return;
                }
            }
            const iron::Vec3 incoming{msg.x, msg.y, msg.z};
            remoteHistories[msg.peerId].push(incoming);
            if (peers.isHost()) {
                // Rebroadcast to all OTHER clients (skip the sender).
                for (std::uint32_t otherPid : peers.peerIds()) {
                    iron::ConnectionId otherConn = peers.connectionFor(otherPid);
                    if (otherConn == c) continue;
                    registry.send<iron::netcubes::PositionMsg>(
                        otherConn, msg, iron::SendReliability::Unreliable);
                }
            }
        });
```

- [ ] **Step 7: Update every other reference to `isHost`, `myPeerId`, `connToPeerId`, `hostConn`**

Search the file for these names. Each should become:
- `isHost` → `peers.isHost()`
- `myPeerId` → `peers.myPeerId()`
- `hostConn` → `peers.connectionFor(0)` (the host's peerId is always 0 from a client's perspective)
- `connToPeerId` (in iteration) → `peers.peerIds()` then `peers.connectionFor(pid)`

The broadcast block in the main loop:

```cpp
        if (haveIdentity) {
            const auto since = ...;
            if (since >= 33) {
                lastSend = now;
                const iron::netcubes::PositionMsg msg{ myId, ... };
                if (isHost) {
                    for (const auto& [c, _] : connToPeerId) {
                        registry.send<iron::netcubes::PositionMsg>(c, msg, ...);
                    }
                } else {
                    registry.send<iron::netcubes::PositionMsg>(hostConn, msg, ...);
                }
            }
        }
```

Becomes:

```cpp
        if (haveIdentity) {
            const auto since = ...;
            if (since >= 33) {
                lastSend = now;
                const iron::netcubes::PositionMsg msg{ myId, ... };
                if (peers.isHost()) {
                    peers.broadcastToAll<iron::netcubes::PositionMsg>(
                        msg, iron::SendReliability::Unreliable);
                } else {
                    peers.send<iron::netcubes::PositionMsg>(
                        0, msg, iron::SendReliability::Unreliable);
                }
            }
        }
```

And local `myId` / `haveIdentity` redeclarations:

```cpp
        const std::uint32_t myId = peers.isHost() ? 0u : peers.myPeerId();
        const bool haveIdentity = peers.hasIdentity();
```

- [ ] **Step 8: Update the per-frame peer count for the HUD**

Find the `peerCount` line. Replace with:

```cpp
        const std::size_t peerCount = (peers.hasIdentity() ? 1u : 0u) + remoteHistories.size();
        hud.setText(peersText, "Peers: " + std::to_string(peerCount));
```

(Unchanged in spirit — `peers.hasIdentity()` replaces the old local `haveIdentity`.)

- [ ] **Step 9: Update `transport.poll()` → `peers.poll()`**

In the main loop, find `transport.poll();` and change to `peers.poll();`.

- [ ] **Step 10: Update `transport.stop()` → `peers.stop()`**

At the end of `main()`, before `return 0;`, change `transport.stop();` to `peers.stop();`.

- [ ] **Step 11: Update the net-stats HUD widget's connection lookup**

Find the `iron::ConnectionId statsConn` block:

```cpp
        iron::ConnectionId statsConn = iron::kInvalidConnection;
        if (isHost) {
            if (!connToPeerId.empty()) statsConn = connToPeerId.begin()->first;
        } else {
            statsConn = hostConn;
        }
```

Replace with:

```cpp
        iron::ConnectionId statsConn = iron::kInvalidConnection;
        if (peers.isHost()) {
            const auto ids = peers.peerIds();
            if (!ids.empty()) statsConn = peers.connectionFor(ids.front());
        } else {
            statsConn = peers.connectionFor(0);
        }
```

- [ ] **Step 12: Build**

```powershell
cmake --build build --target net-cubes
```

Use `timeout: 180000`. Expected: clean build.

- [ ] **Step 13: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 23/23 pass.

- [ ] **Step 14: Smoke (skip if headless)**

```powershell
./build/games/05-net-cubes/Debug/net-cubes.exe
./build/games/05-net-cubes/Debug/net-cubes.exe --connect 127.0.0.1
```

Same behaviour as M8.4: cubes sync smoothly, HUD shows role/peers/net-stats, ESC releases cursor, click recaptures, cross-game connect to net-tag still errors out cleanly.

- [ ] **Step 15: Commit**

```powershell
git add games/05-net-cubes/Messages.h games/05-net-cubes/main.cpp
git commit -m "Net-cubes: adopt PeerManager (delete hand-rolled HelloMsg + bookkeeping)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 4: Refactor `games/06-net-tag` onto PeerManager + PredictionEngine

**Files:**
- Modify: `games/06-net-tag/Messages.h`
- Modify: `games/06-net-tag/main.cpp`

The bigger refactor. Three concerns:
- PeerManager (same as cubes Task 3)
- New PlayerInputMsg + AuthorityPositionMsg replacing the old PositionMsg
- PredictionEngine for local-player position; host applies inputs to authoritative state

- [ ] **Step 1: Replace `games/06-net-tag/Messages.h`**

Use Write. Replace the entire file with:

```cpp
#pragma once

#include <cstdint>

namespace iron::nettag {

// 4-byte ASCII game identifier. 't', 'A', 'G', 'o' → 0x7441'476F.
constexpr std::uint32_t kGameId = 0x7441476Fu;

// Client → host each input frame. dx/dy/dz are movement delta in world
// coordinates already (client has computed from WASD + yaw).
struct PlayerInputMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t inputId;
    float dx, dy, dz;
};

// Host → all peers; broadcasts the authoritative position. The
// lastInputId field lets the sender's client reconcile its prediction.
// lastInputId is 0 for the host's own peer (no client to reconcile).
struct AuthorityPositionMsg {
    static constexpr std::uint8_t kTag = 3;
    std::uint32_t peerId;
    float x, y, z;
    std::uint32_t lastInputId;
};

struct TagSwapMsg {
    static constexpr std::uint8_t kTag = 4;
    std::uint32_t newItPeerId;
};

struct ScoreUpdateMsg {
    static constexpr std::uint8_t kTag = 5;
    std::uint32_t peerId;
    float itTimeSec;
};

struct RoundStartMsg {
    static constexpr std::uint8_t kTag = 6;
    std::uint32_t initialItPeerId;
    float roundDurationSec;
};

struct RoundEndMsg {
    static constexpr std::uint8_t kTag = 7;
    std::uint32_t winnerPeerId;
};

}  // namespace iron::nettag
```

(`HelloMsg` is gone — PeerManager owns it. `PositionMsg` is replaced by `PlayerInputMsg` + `AuthorityPositionMsg`. `TagSwap`, `ScoreUpdate`, `RoundStart`, `RoundEnd` all bumped up by 1.)

- [ ] **Step 2: Update `games/06-net-tag/main.cpp` — includes**

Use Edit. Add after the existing `#include "net/TimeHistory.h"` line:

```cpp
#include "net/PeerManager.h"
#include "net/PredictionEngine.h"
```

- [ ] **Step 3: Add the simulate function + PlayerState/PlayerInput types**

Insert these near the top of `main()`, before the player declaration:

```cpp
    // Server-authoritative simulation. Client and host both run this on
    // the same inputs in the same order, so prediction should always
    // match (PredictionEngine's reconcile is exercised but won't fire
    // in this trivial sim). When the sim becomes non-trivial (M8.6
    // shooter with collisions), reconcile will actually do work.
    struct PlayerState { float x, y, z; };
    struct PlayerInput { float dx, dy, dz; };
    auto simulate = [](const PlayerState& s, const PlayerInput& i, float /*dt*/) {
        return PlayerState{s.x + i.dx, s.y + i.dy, s.z + i.dz};
    };
```

- [ ] **Step 4: Replace `player` (the Player struct) and add PredictionEngine**

The existing `player` struct holds {position, yaw, pitch}. We keep yaw/pitch on a small local struct, but `position` moves into the PredictionEngine.

Find:
```cpp
    Player player;
    player.position = iron::Vec3{8.0f, 4.0f, 12.0f};
    ...
```

Replace with:

```cpp
    // yaw/pitch are camera-orientation state owned locally (server
    // doesn't care about look direction). Position lives in the
    // predictor below.
    struct LookState { float yaw = -0.5f; float pitch = -0.25f; } look;

    iron::PredictionEngine<PlayerInput, PlayerState> predictor{
        simulate,
        /*fixedDt*/ 1.0f / 60.0f,
        /*initial*/ PlayerState{8.0f, 4.0f, 12.0f}};
```

Anywhere that read `player.position`, replace with a small helper:

```cpp
    auto myPos = [&]() {
        const auto& s = predictor.predictedState();
        return iron::Vec3{s.x, s.y, s.z};
    };
```

Anywhere that read `player.yaw` or `player.pitch`, replace with `look.yaw` / `look.pitch`.

- [ ] **Step 5: Remove the old peer-state declarations**

Find:
```cpp
    bool isHost = false;
    iron::ConnectionId hostConn = iron::kInvalidConnection;
    std::uint32_t myPeerId = 0;
    std::unordered_map<iron::ConnectionId, std::uint32_t> connToPeerId;
    std::uint32_t nextPeerId = 1;
```

DELETE.

- [ ] **Step 6: Replace transport/registry/connection-callback setup with PeerManager**

Find and DELETE all of these blocks:
- The `transport.setOnConnectionOpened` lambda
- The `transport.setOnConnectionClosed` lambda
- The `registerHandler<iron::nettag::HelloMsg>` lambda

After the `iron::MessageRegistry registry(&transport);` line, INSERT:

```cpp
    iron::PeerManager peers(transport, registry, iron::nettag::kGameId);

    // Host-side authoritative state per peer.
    std::unordered_map<std::uint32_t, PlayerState> authStates;

    peers.setOnPeerJoined([&](std::uint32_t pid) {
        // Host: initialise per-peer game state (PlayerInfo, authoritative
        // position). Clients ignore this (just the host runs sim).
        if (peers.isHost()) {
            if (players.find(pid) == players.end()) players[pid] = PlayerInfo{};
            // Authoritative position: host's own peer uses local predictor
            // state; remote peers default to spawn (they'll move as soon
            // as their inputs arrive).
            if (authStates.find(pid) == authStates.end()) {
                if (pid == 0) {
                    authStates[pid] = predictor.predictedState();
                } else {
                    authStates[pid] = PlayerState{0.0f, 0.5f, 0.0f};
                }
            }
            // Late-joiner snapshot (was in M8.3 onConnectionOpened):
            // send RoundStartMsg + current scores if there's a round.
            if (roundActive && pid != 0) {
                const iron::nettag::RoundStartMsg snapshot{
                    itPeerId, std::max(0.0f, roundTimeRemainingSec)};
                peers.send(pid, snapshot, iron::SendReliability::Reliable);
                for (const auto& [otherPid, info] : players) {
                    peers.send(pid,
                        iron::nettag::ScoreUpdateMsg{otherPid, info.itTimeAccumSec},
                        iron::SendReliability::Reliable);
                }
            }
        }
    });

    peers.setOnPeerLeft([&](std::uint32_t pid) {
        remoteHistories.erase(pid);
        if (peers.isHost()) {
            authStates.erase(pid);
            // Existing on-it-disconnects logic still lives in the host
            // gameplay tick (compares players map against connToPeerId
            // — that loop should be updated to use peers.peerIds()).
        } else {
            // Host disconnected — exit.
            glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
        }
    });
```

- [ ] **Step 7: Replace `transport.start()` + listen/connect logic with `peers.start(netArgs)`**

Same as cubes Task 3 Step 5: find and delete the existing `transport.start()` + listen/connect/fallback block. Replace with:

```cpp
    if (!peers.start(netArgs)) {
        iron::Log::error("net-tag: PeerManager.start failed");
        return 1;
    }
```

- [ ] **Step 8: Replace the position-message handler with a PlayerInputMsg handler (host) + AuthorityPositionMsg handler (everyone)**

DELETE the existing `registry.registerHandler<iron::nettag::PositionMsg>(...)` block entirely.

ADD two new handlers right where it was:

```cpp
    // Host: client input arrives → apply to that client's authoritative
    // state → broadcast new AuthorityPositionMsg.
    registry.registerHandler<iron::nettag::PlayerInputMsg>(
        [&](iron::ConnectionId c, const iron::nettag::PlayerInputMsg& msg) {
            if (!peers.isHost()) return;
            auto pid = peers.peerIdFor(c);
            if (!pid) return;
            authStates[*pid] = simulate(
                authStates[*pid], PlayerInput{msg.dx, msg.dy, msg.dz}, 0.0f);
            const auto& s = authStates[*pid];
            peers.broadcastToAll<iron::nettag::AuthorityPositionMsg>(
                iron::nettag::AuthorityPositionMsg{
                    *pid, s.x, s.y, s.z, msg.inputId},
                iron::SendReliability::Unreliable);
        });

    // All peers: receive authoritative position.
    //   - For our own peerId: reconcile the predictor against authState.
    //   - For other peers: push into the TimeHistory<Vec3> (existing flow).
    registry.registerHandler<iron::nettag::AuthorityPositionMsg>(
        [&](iron::ConnectionId, const iron::nettag::AuthorityPositionMsg& msg) {
            if (msg.peerId == (peers.isHost() ? 0u : peers.myPeerId())) {
                if (!peers.isHost()) {
                    predictor.reconcile(
                        PlayerState{msg.x, msg.y, msg.z}, msg.lastInputId);
                }
                // (Host doesn't need to reconcile its own state; it IS
                //  authoritative.)
            } else {
                remoteHistories[msg.peerId].push(iron::Vec3{msg.x, msg.y, msg.z});
            }
        });
```

- [ ] **Step 9: Update the per-frame input + broadcast block**

The existing block computes a movement delta and sends a `PositionMsg`. We need to:
1. Compute `PlayerInput{dx, dy, dz}` from current WASD/QE/yaw.
2. `applyInput` on the predictor (advances local predicted state).
3. If host: also apply to `authStates[0]` AND broadcast authority.
4. If client: send `PlayerInputMsg{inputId, dx, dy, dz}` to host.

Find the existing chase-cam + input + broadcast block in the main loop and reorganize. The relevant portion currently looks like:

```cpp
        // ... yaw/pitch from mouse, look direction ...
        if (kW) player.position += forward * speed * dt;
        if (kS) player.position -= forward * speed * dt;
        // ... etc ...

        // Broadcast our position ~30 Hz.
        if (haveIdentity) {
            if (since >= 33ms) {
                const iron::nettag::PositionMsg msg{ myId, player.position.x, ... };
                if (isHost) broadcastToAll(); else sendToHost();
            }
        }
```

Replace the position-update section with input-then-applyInput. Use the FixedTickScheduler that the spec called for (~30Hz input rate). DECLARE the input ticker once near `scoreTicker`:

```cpp
    iron::FixedTickScheduler inputTicker{std::chrono::milliseconds{33}};  // ~30 Hz
```

And REPLACE the per-frame movement code:

```cpp
        // Movement input: convert WASD/QE into world-space delta using
        // current yaw, fire one input per fixed tick. dt-from-frame isn't
        // used directly; we accumulate input direction and emit ONCE per
        // tick with a fixed speed-per-tick.
        const float yawSin = std::sin(look.yaw);
        const float yawCos = std::cos(look.yaw);
        const iron::Vec3 forwardXZ = iron::normalize(iron::Vec3{-yawSin, 0.0f, -yawCos});
        const iron::Vec3 rightXZ   = iron::normalize(iron::Vec3{ yawCos, 0.0f, -yawSin});
        const float speedPerTick = kMoveSpeed / 30.0f;  // matches inputTicker rate
        iron::Vec3 dirThisFrame{0,0,0};
        if (kW) dirThisFrame = dirThisFrame + forwardXZ;
        if (kS) dirThisFrame = dirThisFrame - forwardXZ;
        if (kD) dirThisFrame = dirThisFrame + rightXZ;
        if (kA) dirThisFrame = dirThisFrame - rightXZ;
        if (kE) dirThisFrame.y += 1.0f;
        if (kQ) dirThisFrame.y -= 1.0f;
        // Normalise XZ diagonal so diagonal isn't faster:
        const float xzLen = std::sqrt(dirThisFrame.x*dirThisFrame.x + dirThisFrame.z*dirThisFrame.z);
        if (xzLen > 1.0f) {
            dirThisFrame.x /= xzLen;
            dirThisFrame.z /= xzLen;
        }

        if (peers.hasIdentity()) {
            inputTicker.update(dt, [&]() {
                PlayerInput in{
                    dirThisFrame.x * speedPerTick,
                    dirThisFrame.y * speedPerTick,
                    dirThisFrame.z * speedPerTick,
                };
                const auto inputId = predictor.applyInput(in);
                if (peers.isHost()) {
                    // Host: apply to its own authoritative state and
                    // broadcast.
                    authStates[0] = simulate(authStates[0], in, 0.0f);
                    peers.broadcastToAll<iron::nettag::AuthorityPositionMsg>(
                        iron::nettag::AuthorityPositionMsg{
                            0, authStates[0].x, authStates[0].y, authStates[0].z,
                            /*lastInputId=*/0},
                        iron::SendReliability::Unreliable);
                } else {
                    // Client: ship input to host.
                    peers.send<iron::nettag::PlayerInputMsg>(
                        0,
                        iron::nettag::PlayerInputMsg{inputId, in.dx, in.dy, in.dz},
                        iron::SendReliability::Unreliable);
                }
            });
        }

        // Local cube position is whatever the predictor says.
        const iron::Vec3 myPosition = myPos();
```

DELETE the old `lastSend` declaration and the old broadcast `if (since >= 33)` block — replaced by the FixedTickScheduler above.

- [ ] **Step 10: Update host's tag-detection block to use authStates**

Find the `latestPosition` helper from M8.4:
```cpp
        auto latestPosition = [&](std::uint32_t pid) -> iron::Vec3 {
            if (pid == 0) return player.position;
            auto it = remoteHistories.find(pid);
            ...
        };
```

Replace with:
```cpp
        auto latestPosition = [&](std::uint32_t pid) -> iron::Vec3 {
            auto it = authStates.find(pid);
            if (it == authStates.end()) return iron::Vec3{1e9f, 0, 0};
            return iron::Vec3{it->second.x, it->second.y, it->second.z};
        };
```

(Host uses `authStates` for tag-detection — it's the source of truth for ALL players including itself.)

- [ ] **Step 11: Update the `latestPosition`-using code paths to handle disconnects**

In the existing "remove player entries for clients that have disconnected" loop (the one that iterates `players` and checks against `connToPeerId`), replace `connToPeerId` lookups with `peers.peerIds()`:

```cpp
            // Sync players against PeerManager's current peer list.
            for (auto it = players.begin(); it != players.end(); ) {
                bool stillConnected = (it->first == 0);  // host always present
                for (std::uint32_t pid : peers.peerIds()) {
                    if (pid == it->first) { stillConnected = true; break; }
                }
                if (!stillConnected) {
                    // ... existing "hand off it if dropped" logic ...
                    authStates.erase(it->first);
                    it = players.erase(it);
                } else {
                    ++it;
                }
            }
```

Also update the "ensure every connected client has a player entry" loop:
```cpp
            // Ensure every connected peer has a player entry.
            for (std::uint32_t pid : peers.peerIds()) {
                if (players.find(pid) == players.end()) {
                    players[pid] = PlayerInfo{};
                    authStates[pid] = PlayerState{0.0f, 0.5f, 0.0f};
                }
            }
```

- [ ] **Step 12: Update broadcast helpers throughout the host gameplay tick**

Find every `for (const auto& [c, _] : connToPeerId) { registry.send<X>(c, ...); }` pattern and replace with `peers.broadcastToAll<X>(msg, reliability);`.

Five such patterns: RoundStartMsg broadcast, TagSwapMsg broadcast, ScoreUpdateMsg broadcast (inside FixedTickScheduler), RoundEndMsg broadcast, and TagSwapMsg in disconnect-it-handoff.

- [ ] **Step 13: Update `transport.poll()` → `peers.poll()` and `transport.stop()` → `peers.stop()`**

In the main loop top, and just before `return 0`.

- [ ] **Step 14: Update the net-stats HUD widget's connection lookup**

Same as cubes Task 3 Step 11:

```cpp
        iron::ConnectionId statsConn = iron::kInvalidConnection;
        if (peers.isHost()) {
            const auto ids = peers.peerIds();
            if (!ids.empty()) statsConn = peers.connectionFor(ids.front());
        } else {
            statsConn = peers.connectionFor(0);
        }
        hud.updateNetworkStats(netStatsHud, transport.stats(statsConn));
```

- [ ] **Step 15: Update remaining references**

Search the file for any leftover:
- `isHost` → `peers.isHost()`
- `myPeerId` → `peers.myPeerId()`
- `hostConn` → `peers.connectionFor(0)` (where applicable)
- `connToPeerId` → use `peers.peerIds()` + `peers.connectionFor(...)`
- `player.position` → `myPos()` (the lambda) or `predictor.predictedState()` field access depending on context

`player.yaw` / `player.pitch` become `look.yaw` / `look.pitch`.

The `haveIdentity` local should become `peers.hasIdentity()`.

The `myId` local should become `peers.isHost() ? 0u : peers.myPeerId()`.

- [ ] **Step 16: Build**

```powershell
cmake --build build --target net-tag
```

Use `timeout: 180000`. Expected: clean build.

- [ ] **Step 17: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 23/23 pass.

- [ ] **Step 18: Smoke (skip if headless)**

Launch two instances. Verify:
- Round timer counts down from 60 on host.
- "It" cube glows red.
- Tag swap on collision (~1.5m, 0.5s cooldown).
- Scoreboard updates each second.
- Round-end → 5s "Winner" → new round starts.
- Movement feels instant locally (PredictionEngine doing its job).
- Remote players smooth (TimeHistory still handles them).

- [ ] **Step 19: Commit**

```powershell
git add games/06-net-tag/Messages.h games/06-net-tag/main.cpp
git commit -m "Net-tag: PeerManager + PredictionEngine + server-authoritative position" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 5: Docs

**Files:**
- Modify: `docs/engine/networking.md`

- [ ] **Step 1: Append new sections to `docs/engine/networking.md`**

Use Edit. Find the last line of the file (should end with text about the network debug HUD) and append:

```markdown

## Peer lifecycle: `iron::PeerManager`

Every networked game has the same boilerplate: peerId assignment,
conn↔peerId map, gameId validation, Hello flow, onPeerJoined /
onPeerLeft handling. `iron::PeerManager` owns it all. Reserves message
**tag=1** for `iron::peer::HelloMsg` — your game's messages start at
tag 2.

```cpp
iron::GnsTransport transport;
iron::MessageRegistry registry(&transport);
iron::PeerManager peers(transport, registry, /*gameId*/ 0xMYGAMEu);

peers.setOnPeerJoined([&](std::uint32_t pid) { initPlayer(pid); });
peers.setOnPeerLeft([&](std::uint32_t pid)   { dropPlayer(pid); });

peers.start(iron::parseNetArgs(argc, argv));

while (running) {
    peers.poll();
    if (peers.isHost()) {
        peers.broadcastToAll(myMsg, iron::SendReliability::Reliable);
    } else {
        peers.send(0, myMsg, iron::SendReliability::Unreliable);
    }
}
```

The host fires `onPeerJoined(0)` synchronously inside `start()` — your
join callback runs once for "me" before any client connects, so
per-peer init has a uniform path.

## Client-side prediction: `iron::PredictionEngine<TInput, TState>`

For server-authoritative games (server runs the simulation, clients
render), the client predicts locally for input responsiveness and
reconciles when authoritative state arrives. Engine handles the input
history + replay; game provides a deterministic `simulate` function:

```cpp
struct PlayerState { float x, y, z; };
struct PlayerInput { float dx, dy, dz; };
auto simulate = [](const PlayerState& s, const PlayerInput& i, float dt) {
    return PlayerState{s.x + i.dx, s.y + i.dy, s.z + i.dz};
};

iron::PredictionEngine<PlayerInput, PlayerState> predictor{
    simulate, /*fixedDt*/ 1.0f / 60.0f, PlayerState{0, 0, 0}};

// Each input tick:
auto inputId = predictor.applyInput(currentInput);
send(PlayerInputMsg{inputId, ...});

// Render:
render(predictor.predictedState());

// When AuthorityMsg arrives:
predictor.reconcile(PlayerState{msg.x, msg.y, msg.z}, msg.lastInputId);
```

If the client's predicted state at `lastInputId` matched the server's
authoritative state, nothing visible happens (just history bookkeeping).
If it didn't match, predicted state snaps to authoritative and any
inputs since are replayed against it. State comparison uses
`TState::operator==` — works for deterministic single-platform
simulations.

## Snapshot pattern (late-joiner state catch-up)

When a peer joins mid-game, they need a snapshot of in-progress world
state. The engine doesn't abstract this — instead each game sends the
right messages from inside the `onPeerJoined` callback. Reference
implementation in net-tag:

```cpp
peers.setOnPeerJoined([&](std::uint32_t newPeer) {
    if (!peers.isHost()) return;
    if (newPeer == 0) return;   // 0 = self (the host fires onJoined(0))

    // Send current round state to the new joiner:
    if (roundActive) {
        peers.send(newPeer,
            iron::nettag::RoundStartMsg{itPeerId, roundTimeRemainingSec},
            iron::SendReliability::Reliable);
        for (const auto& [pid, info] : players) {
            peers.send(newPeer,
                iron::nettag::ScoreUpdateMsg{pid, info.itTimeAccumSec},
                iron::SendReliability::Reliable);
        }
    }
});
```

Use `Reliable` for snapshot — late-joiners can't miss it.
```

- [ ] **Step 2: Commit**

```powershell
git add docs/engine/networking.md
git commit -m "Docs: PeerManager + PredictionEngine + snapshot pattern (M8.5)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 6: Code review + PR

**Files:** none modified — read-only review (plus fixes the review surfaces)

- [ ] **Step 1: Show the diff range**

```powershell
git log --oneline main..HEAD
git diff main --stat
```

Expected: 5 commits (Tasks 1–5).

- [ ] **Step 2: Build + full ctest + smoke-run both refactored exes**

```powershell
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
./build/games/05-net-cubes/Debug/net-cubes.exe  # close after 2s
./build/games/06-net-tag/Debug/net-tag.exe       # close after 2s
```

Use `timeout: 300000` on the build. Expected: clean build; 23/23 pass; both exes launch.

- [ ] **Step 3: Dispatch code-quality reviewer**

Dispatch `feature-dev:code-reviewer` (or `general-purpose`) with this prompt:

> Review the M8.5 prediction-and-helpers changes (`git diff main`) in the Iron Core Engine. Milestone adds `iron::PeerManager` (owns Hello + peer lifecycle; reserves tag=1 for peer::HelloMsg), `iron::PredictionEngine<TInput, TState>` (client-side prediction + reconciliation), refactors net-cubes to use PeerManager, refactors net-tag to use PeerManager + PredictionEngine + server-authoritative position (PlayerInputMsg + AuthorityPositionMsg replace the old PositionMsg). Spec: `docs/superpowers/specs/2026-05-24-prediction-and-helpers-design.md`.
>
> Focus on:
> 1. **PeerManager safety** — destructor unhooks transport callbacks; gameId mismatch closes the connection cleanly; onPeerJoined(0) fires synchronously inside start() for the host; client receives onPeerJoined for self AND host (peerId 0).
> 2. **PredictionEngine correctness** — applyInput advances state and history; reconcile match drops confirmed entries; reconcile mismatch snaps + replays; stale confirmation ignored; reset wipes.
> 3. **Cubes refactor parity** — observable behavior unchanged from M8.4 (gameId reject, smooth remote cubes, host validates spoofed peerId, ESC, chase camera). ~80 lines deleted.
> 4. **Tag refactor correctness** — server-authoritative position works (client sends PlayerInputMsg, host applies + broadcasts AuthorityPositionMsg, client reconciles); tag-detection uses authoritative state (`authStates`); late-joiner snapshot now lives in onPeerJoined; "it" disconnect handoff still works; all M8.3+M8.4 bug fixes preserved (cooldown both players, host-guard on client-bound msgs, "(waiting for round)" HUD, broadcast loop never freezes).
> 5. **Wire-format change** — tags renumbered: PlayerInputMsg=2, AuthorityPositionMsg=3, TagSwapMsg=4, ScoreUpdateMsg=5, RoundStartMsg=6, RoundEndMsg=7. Both client and host of M8.5 use the new format. No stale references to old `PositionMsg::kTag = 2`.
> 6. **Header hygiene** — game code does NOT include `<steam/...>` headers.
>
> Skip style nits. Cap at 10 findings. Under 400 words. End with **APPROVED** or **NEEDS_FIXES**.

- [ ] **Step 4: Address findings**

Apply fixes. Push back on cosmetic suggestions; only block on real correctness issues.

- [ ] **Step 5: Final verification**

```powershell
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, 23/23 pass.

- [ ] **Step 6: Commit review fixes (skip if none)**

```powershell
git add -A
git commit -m "M8.5: address code-review findings" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

- [ ] **Step 7: Push and open the PR**

```powershell
git push -u origin feat/prediction-and-helpers
gh pr create --title "Milestone 8.5: Prediction stack + helpers (PeerManager + PredictionEngine)" --body "$(cat <<'EOF'
## Summary

Two engine helpers that take networked games from ~150 lines of boilerplate per game to ~5:

- **`iron::PeerManager`** (engine/net) — owns Hello + gameId validation + conn↔peerId map + onPeerJoined/Left lifecycle. Reserves message tag=1 for `iron::peer::HelloMsg`; games' MessageRegistry tags start at 2.
- **`iron::PredictionEngine<TInput, TState>`** (engine/net) — header-only template for client-side input prediction + reconciliation. Game provides a deterministic `simulate(state, input, dt)` function; engine handles input history + replay-on-mismatch.

Both networked demos refactored:

- **net-cubes** (~80 lines deleted) — adopts PeerManager. Wire format unchanged (still client-authoritative position).
- **net-tag** — adopts PeerManager + PredictionEngine + **server-authoritative position**. Client sends `PlayerInputMsg`; host applies + broadcasts `AuthorityPositionMsg`. Client uses PredictionEngine for local-player responsiveness (reconciliation is exercised but never fires in this trivial sim — it will fire for real in M8.6 hero shooter with collisions).

Snapshot pattern documented (`docs/engine/networking.md`) with net-tag's `onPeerJoined`-driven late-joiner snapshot as the reference impl. No engine abstraction — game state varies too much.

## Test plan

- [x] `test_peer_manager` passes (5 cases: paired connect, gameId mismatch, send/broadcast, disconnect, accessors)
- [x] `test_prediction_engine` passes (5 cases: apply, match, mismatch, stale, reset)
- [x] All 23 unit tests pass locally
- [x] Both games build + launch
- [x] Manual: cubes/tag observable behavior unchanged (round timer, tag swap, scoring, ESC, chase camera, smooth remote cubes, cross-connect rejected)
- [x] Manual: net-tag's local-player movement still feels instant (PredictionEngine working)

## Wire format break (net-tag only)

Tags renumbered + message types replaced:

| Message | M8.4 tag | M8.5 tag |
|---------|----------|----------|
| (engine) `peer::HelloMsg` | — | 1 |
| `nettag::HelloMsg` | 1 | DELETED |
| `nettag::PlayerInputMsg` | — | 2 (NEW) |
| `nettag::AuthorityPositionMsg` | — | 3 (NEW) |
| `nettag::PositionMsg` | 2 | DELETED |
| `nettag::TagSwapMsg` | 3 | 4 |
| `nettag::ScoreUpdateMsg` | 4 | 5 |
| `nettag::RoundStartMsg` | 5 | 6 |
| `nettag::RoundEndMsg` | 6 | 7 |

Old M8.4 net-tag clients/hosts cannot interop with M8.5. Acceptable; no installed base.

## Out of scope (M8.6+)

- Hero shooter game using all of this
- Lag-compensation server-side use of TimeHistory
- `SnapshotBroadcaster<T>` engine abstraction (pattern only, no helper)
- Reconnect / host migration
- Tolerance comparator for PredictionEngine (current uses exact `operator==`)

EOF
)"
```

Return the PR URL.

---

## Self-review (run after writing the plan, before handoff)

Already done inline:

- **Spec coverage:**
  - `iron::peer::HelloMsg` (tag=1 reserved) → Task 1 Step 3
  - `iron::PeerManager` + 5 test cases → Task 1
  - `iron::PredictionEngine<TInput, TState>` + 5 test cases → Task 2
  - Net-cubes adopts PeerManager (cleanup only) → Task 3
  - Net-tag adopts PeerManager + PredictionEngine + server-auth position → Task 4
  - Wire format: PlayerInputMsg=2, AuthorityPositionMsg=3, renumbered TagSwap+→4-7 → Task 4 Step 1
  - Late-joiner snapshot moves into onPeerJoined → Task 4 Step 6
  - Snapshot pattern docs → Task 5
  - Code review → Task 6
- **Non-goals respected:** no SnapshotBroadcaster, no lag-comp game-side, no tolerance comparator, no hero shooter, no reconnect.
- **Placeholder scan:** no TBD/TODO. Wire-format table makes tag renumbering unambiguous.
- **Type consistency:**
  - `iron::peer::HelloMsg`, `iron::PeerManager` methods (start/stop/isHost/myPeerId/hasIdentity/peerIds/connectionFor/peerIdFor/send/broadcastToAll/poll/setOnPeerJoined/setOnPeerLeft) consistent across header, impl, tests, and both game refactors.
  - `iron::PredictionEngine<>` API (applyInput/predictedState/reconcile/historySize/reset) consistent across header, tests, and net-tag.
  - `iron::nettag::{PlayerInputMsg, AuthorityPositionMsg, TagSwapMsg, ScoreUpdateMsg, RoundStartMsg, RoundEndMsg}` with their new kTag values consistent across Messages.h, send sites, and handler registration.
  - `PlayerState{x,y,z}` and `PlayerInput{dx,dy,dz}` consistent across simulate, predictor, handlers, and broadcast.
