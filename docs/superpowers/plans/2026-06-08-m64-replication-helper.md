# M64 — Reusable Authoritative Replication Helper Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a generic engine-level `Replicator` (authoritative whole-object replication + client→server commands + RepNotify + late-join) on top of the existing `engine/net/` stack, plus the `ByteStream` serialization layer and a raw channel on `MessageRegistry` it needs.

**Architecture:** Three units. `ByteStream` (header-only `ByteWriter`/`ByteReader` + ADL `serialize`/`deserialize` with a POD default). `MessageRegistry` gains a variable-length raw channel (`sendRaw`/`registerRawHandler`). `Replicator` (built on `PeerManager` + `MessageRegistry`) registers replicated objects, broadcasts dirty ones on `flush()` reliably, dispatches client commands to host handlers, and sends full state to late-joiners — all under one reserved message tag (250), sub-dispatched internally.

**Tech Stack:** C++17, the existing `engine/net/` (NetTransport + MockTransport/GnsTransport, MessageRegistry, PeerManager), custom `tests/test_framework.h`, GameNetworkingSockets (production transport, only exercised by the demo).

**Reference files (read before starting):**
- `engine/net/NetTransport.h` — `ConnectionId`, `kInvalidConnection`, `NetAddress`, `SendReliability{Reliable,Unreliable}`, the transport interface.
- `engine/net/MessageRegistry.h` (+ `.cpp`) — `kMaxPayloadBytes=1200`, the POD `send`/`registerHandler` templates, private `handlers_` map (`unordered_map<uint8_t, function<void(ConnectionId, span<const std::byte>)>>`) and `dispatch()`. **Read `.cpp` to confirm `dispatch` passes the post-tag payload (subspan(1)) to the handler.**
- `engine/net/PeerManager.h` — `isHost()`, `myPeerId()`, `peerIds()`, `connectionFor(peerId)`, `peerIdFor(conn)`, `send<Msg>`, `broadcastToAll<Msg>`, `setOnPeerJoined`. Host peerId is 0; on a client `connectionFor(0)` is the host connection.
- `engine/net/backends/mock/MockTransport.h` — in-process loopback for tests (FIFO delivery, reliability not modelled).
- `tests/test_peer_manager.cpp` — **the canonical two-node MockTransport harness**: `MockTransport srvT,cliT; MessageRegistry srvR(&srvT),cliR(&cliT); PeerManager srv(srvT,srvR,gameId); PeerManager cli(cliT,cliR,gameId);` then `start()` + pump `srv.poll();cli.poll();` a few times to connect. Model `test_replicator.cpp` on it.
- `engine/core/NetArgs.h` — `NetArgs{addr,wantsConnect}`, `parseNetArgs(argc,argv)`.
- `games/05-net-cubes/main.cpp` + `CMakeLists.txt` — the simplest networked-game scaffold to model the Task 6 demo on.
- The design spec: `docs/superpowers/specs/2026-06-08-m64-replication-helper-design.md`.

**Build/test commands (Windows, canonical build dir `build-vk`):**
- Build a test: `cmake --build build-vk --target test_replicator`
- Reconfigure after adding files/targets: `cmake -S . -B build-vk`
- Run a test: `build-vk\tests\Debug\test_replicator.exe` → prints `OK - all checks passed`, exit 0.
- **CI-readiness (per `[[verify-clean-build-before-ci]]`):** a full build is `cmake --build build-vk`; verify its **exit code is 0** and grep the log for `error C/error LNK/fatal error` — do NOT trust the truncated tail, and do NOT trust a test run that may have executed a stale `.exe` from a build that actually failed.

---

## File Structure

**Create:**
- `engine/net/ByteStream.h` — header-only serialization primitives + POD default.
- `engine/net/Replicator.h` / `engine/net/Replicator.cpp` — the helper.
- `tests/test_byte_stream.cpp` — ByteStream round-trip + safety.
- `tests/test_replicator.cpp` — Replicator over MockTransport.
- `games/14-net-repl-demo/main.cpp` / `CMakeLists.txt` — smoke demo (replicated counter).

**Modify:**
- `engine/net/MessageRegistry.h` / `.cpp` — add `registerRawHandler` + `sendRaw`.
- `tests/test_message_registry.cpp` — add a raw-channel round-trip case.
- `engine/CMakeLists.txt` — add `net/Replicator.cpp`.
- `tests/CMakeLists.txt` — add `test_byte_stream`, `test_replicator`.
- `CMakeLists.txt` — `add_subdirectory(games/14-net-repl-demo)`.

**Reserved-tag convention (document in code comments):** tag 1 = Hello (PeerManager), **250 = replication (Replicator)**, 254/255 = ping/pong (PeerManager). Games use tags **2–249**.

---

## Task 1: ByteStream serialization layer

**Files:**
- Create: `engine/net/ByteStream.h`
- Test: `tests/test_byte_stream.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test** — `tests/test_byte_stream.cpp`

```cpp
#include "net/ByteStream.h"
#include "test_framework.h"

#include <cstdint>
#include <vector>

using namespace iron;

namespace {
// A non-POD type with a hand-written serializer (the interesting case).
struct Bag { std::vector<int> items; };
void serialize(ByteWriter& w, const Bag& b) {
    w.u32(static_cast<std::uint32_t>(b.items.size()));
    for (int v : b.items) w.i32(v);
}
void deserialize(ByteReader& r, Bag& b) {
    const std::uint32_t n = r.u32();
    b.items.clear();
    for (std::uint32_t i = 0; i < n && !r.failed(); ++i) b.items.push_back(r.i32());
}
// A POD type uses the default template serializer (no hand-written functions).
struct Pod { int a; float b; std::uint8_t c; };
}  // namespace

int main() {
    // Primitive round-trip.
    {
        ByteWriter w;
        w.u8(0xAB); w.u16(0x1234); w.u32(0xDEADBEEFu); w.i32(-42); w.f32(1.5f); w.boolean(true);
        ByteReader r(w.data());
        CHECK(r.u8() == 0xAB);
        CHECK(r.u16() == 0x1234);
        CHECK(r.u32() == 0xDEADBEEFu);
        CHECK(r.i32() == -42);
        CHECK(r.f32() == 1.5f);
        CHECK(r.boolean() == true);
        CHECK(!r.failed());
        CHECK(r.remaining() == 0);
    }

    // Non-POD round-trip via ADL serialize/deserialize.
    {
        Bag in; in.items = {1, 2, 3, 99};
        ByteWriter w; serialize(w, in);
        ByteReader r(w.data());
        Bag out; deserialize(r, out);
        CHECK(!r.failed());
        CHECK(out.items.size() == 4u);
        CHECK(out.items[3] == 99);
    }

    // POD default template round-trip (no hand-written serializer).
    {
        Pod in{7, 2.5f, 9};
        ByteWriter w; serialize(w, in);
        ByteReader r(w.data());
        Pod out{}; deserialize(r, out);
        CHECK(!r.failed());
        CHECK(out.a == 7 && out.b == 2.5f && out.c == 9);
    }

    // Underflow is safe: reading past the end sets failed() and returns zero,
    // never reads out of bounds.
    {
        ByteWriter w; w.u8(1);
        ByteReader r(w.data());
        CHECK(r.u8() == 1);
        const std::uint32_t bad = r.u32();   // only 0 bytes left
        CHECK(bad == 0u);
        CHECK(r.failed());
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --target test_byte_stream`
Expected: FAIL — `ByteStream.h` / target missing.

- [ ] **Step 3: Write `engine/net/ByteStream.h`**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace iron {

// Append-only byte buffer for network serialization. Little-endian-agnostic in
// the sense that both ends run the same code; primitives are written via memcpy
// of their native representation (all our peers are same-arch x86-64 today).
class ByteWriter {
public:
    void u8(std::uint8_t v)  { put(&v, sizeof(v)); }
    void u16(std::uint16_t v){ put(&v, sizeof(v)); }
    void u32(std::uint32_t v){ put(&v, sizeof(v)); }
    void i32(std::int32_t v) { put(&v, sizeof(v)); }
    void f32(float v)        { put(&v, sizeof(v)); }
    void boolean(bool v)     { std::uint8_t b = v ? 1 : 0; put(&b, sizeof(b)); }
    void raw(std::span<const std::byte> bytes) {
        buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    }

    const std::vector<std::byte>& data() const { return buf_; }

private:
    void put(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<std::byte> buf_;
};

// Reads primitives back from a byte span with bounds checking. On underflow it
// sets a sticky failed() flag and returns zeroed values — callers check failed()
// and abort cleanly; never reads out of bounds.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> bytes) : buf_(bytes) {}

    std::uint8_t  u8()  { std::uint8_t v{};  get(&v, sizeof(v)); return v; }
    std::uint16_t u16() { std::uint16_t v{}; get(&v, sizeof(v)); return v; }
    std::uint32_t u32() { std::uint32_t v{}; get(&v, sizeof(v)); return v; }
    std::int32_t  i32() { std::int32_t v{};  get(&v, sizeof(v)); return v; }
    float         f32() { float v{};         get(&v, sizeof(v)); return v; }
    bool          boolean() { std::uint8_t b{}; get(&b, sizeof(b)); return b != 0; }

    std::size_t remaining() const { return failed_ ? 0 : buf_.size() - pos_; }
    bool failed() const { return failed_; }

private:
    void get(void* p, std::size_t n) {
        if (failed_ || pos_ + n > buf_.size()) { failed_ = true; std::memset(p, 0, n); return; }
        std::memcpy(p, buf_.data() + pos_, n);
        pos_ += n;
    }
    std::span<const std::byte> buf_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

// Default (de)serialization for trivially-copyable types — POD command/value
// structs get serialization for free. Non-POD types (e.g. Inventory) provide
// their own serialize/deserialize overloads, which win via overload resolution
// (non-template exact match beats this template; and SFINAE removes this for
// non-trivially-copyable types anyway).
template <typename T>
std::enable_if_t<std::is_trivially_copyable_v<T>>
serialize(ByteWriter& w, const T& v) {
    w.raw(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&v), sizeof(T)));
}

template <typename T>
std::enable_if_t<std::is_trivially_copyable_v<T>>
deserialize(ByteReader& r, T& v) {
    // Read sizeof(T) bytes (bounds-checked) directly into v.
    std::byte tmp[sizeof(T)];
    for (std::size_t i = 0; i < sizeof(T); ++i) tmp[i] = std::byte{r.u8()};
    if (!r.failed()) std::memcpy(&v, tmp, sizeof(T));
}

}  // namespace iron
```

- [ ] **Step 4: Register the test** in `tests/CMakeLists.txt` (near the other net tests, e.g. after `test_message_registry`):
```cmake
iron_add_test(test_byte_stream test_byte_stream.cpp)
```

- [ ] **Step 5: Build + run to verify it passes**

Run: `cmake --build build-vk --target test_byte_stream` then `build-vk\tests\Debug\test_byte_stream.exe`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/net/ByteStream.h tests/test_byte_stream.cpp tests/CMakeLists.txt
git commit -m "M64: ByteStream serialization layer (ByteWriter/ByteReader + POD default) + tests"
```

---

## Task 2: MessageRegistry raw variable-length channel

**Files:**
- Modify: `engine/net/MessageRegistry.h`, `engine/net/MessageRegistry.cpp`
- Test: `tests/test_message_registry.cpp` (append a case in `main`, before `return iron_test_result();`)

- [ ] **Step 1: Write the failing test** — append to `tests/test_message_registry.cpp`

```cpp
    // M64: raw variable-length channel round-trips an arbitrary payload under a tag.
    {
        MockTransport aT, bT;
        MessageRegistry aR(&aT), bR(&bT);
        // Wire them up directly (no PeerManager needed for this low-level test).
        aT.start(); bT.start();
        aT.listen(NetAddress{0x7F000001u, 7001});
        const ConnectionId aToB = bT.connect(NetAddress{0x7F000001u, 7001});
        for (int i = 0; i < 4; ++i) { aT.poll(); bT.poll(); }

        std::vector<std::byte> got;
        aR.registerRawHandler(200, [&](ConnectionId, std::span<const std::byte> p) {
            got.assign(p.begin(), p.end());
        });

        const std::byte payload[5] = {std::byte{1}, std::byte{2}, std::byte{3},
                                      std::byte{4}, std::byte{5}};
        CHECK(bR.sendRaw(aToB, 200, std::span<const std::byte>(payload, 5),
                         SendReliability::Reliable));
        aT.poll();
        CHECK(got.size() == 5u);
        CHECK(got[0] == std::byte{1});
        CHECK(got[4] == std::byte{5});
    }
```
(Ensure `test_message_registry.cpp` includes `<span>`, `<vector>`, `"net/backends/mock/MockTransport.h"`, `"net/NetTransport.h"` — add any that are missing.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --target test_message_registry`
Expected: FAIL — `registerRawHandler`/`sendRaw` undeclared.

- [ ] **Step 3: Declare the raw API** in `engine/net/MessageRegistry.h` (public section, after `sendToAll`):

```cpp
    // --- raw variable-length channel ---
    // For payloads that aren't fixed-size POD (e.g. the Replicator's snapshots).
    // The handler receives the payload AFTER the tag byte. Wire format is the
    // same [u8 tag][payload...]; raw tags share the same tag namespace as POD
    // messages, so don't reuse a tag for both.
    void registerRawHandler(std::uint8_t tag,
                            std::function<void(ConnectionId, std::span<const std::byte>)> fn);
    bool sendRaw(ConnectionId conn, std::uint8_t tag,
                 std::span<const std::byte> payload, SendReliability reliability);
```

- [ ] **Step 4: Define them** in `engine/net/MessageRegistry.cpp` (add `#include <vector>` if missing):

```cpp
void MessageRegistry::registerRawHandler(
        std::uint8_t tag,
        std::function<void(ConnectionId, std::span<const std::byte>)> fn) {
    // The handlers_ map already stores exactly this signature; a raw handler is
    // just one without the POD-memcpy wrapper. dispatch() passes the post-tag
    // payload through unchanged.
    handlers_[tag] = std::move(fn);
}

bool MessageRegistry::sendRaw(ConnectionId conn, std::uint8_t tag,
                              std::span<const std::byte> payload,
                              SendReliability reliability) {
    if (conn == kInvalidConnection) return false;
    if (1 + payload.size() > kMaxPayloadBytes) {
        Log::warn("MessageRegistry: sendRaw tag=%u payload %zu bytes exceeds max %zu; dropped",
                  static_cast<unsigned>(tag), payload.size(), kMaxPayloadBytes);
        return false;
    }
    std::vector<std::byte> buf;
    buf.reserve(1 + payload.size());
    buf.push_back(std::byte{tag});
    buf.insert(buf.end(), payload.begin(), payload.end());
    return transport_->send(conn, std::span<const std::byte>(buf.data(), buf.size()), reliability);
}
```
(If `dispatch()` in the `.cpp` does NOT already strip the tag and pass `bytes.subspan(1)`, fix the handler invocation so raw handlers receive the post-tag payload — confirm against the existing POD handler path, which already relies on this.)

- [ ] **Step 5: Build + run to verify it passes**

Run: `cmake --build build-vk --target test_message_registry` then `build-vk\tests\Debug\test_message_registry.exe`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/net/MessageRegistry.h engine/net/MessageRegistry.cpp tests/test_message_registry.cpp
git commit -m "M64: MessageRegistry raw variable-length channel (sendRaw/registerRawHandler) + test"
```

---

## Task 3: Replicator — replicated objects (sync + RepNotify)

**Files:**
- Create: `engine/net/Replicator.h`, `engine/net/Replicator.cpp`
- Test: `tests/test_replicator.cpp`
- Modify: `engine/CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test** — `tests/test_replicator.cpp` (state-sync cases; commands/late-join added in Tasks 4–5)

```cpp
#include "net/Replicator.h"
#include "net/ByteStream.h"
#include "net/MessageRegistry.h"
#include "net/PeerManager.h"
#include "net/backends/mock/MockTransport.h"
#include "core/NetArgs.h"
#include "test_framework.h"

#include <cstdint>
#include <vector>

using namespace iron;

namespace {
constexpr std::uint32_t kGame = 0x6400AAAAu;
constexpr NetAddress kAddr{0x7F000001u, 6400};
constexpr ReplicationId kScoreId = 1;

// Non-POD replicated state (vector) to exercise the serialization path.
struct Scoreboard { std::vector<int> scores; };
void serialize(ByteWriter& w, const Scoreboard& s) {
    w.u32(static_cast<std::uint32_t>(s.scores.size()));
    for (int v : s.scores) w.i32(v);
}
void deserialize(ByteReader& r, Scoreboard& s) {
    const std::uint32_t n = r.u32();
    s.scores.clear();
    for (std::uint32_t i = 0; i < n && !r.failed(); ++i) s.scores.push_back(r.i32());
}

NetArgs hostArgs() { NetArgs a; a.addr = kAddr; a.wantsConnect = false; return a; }
NetArgs clientArgs() { NetArgs a; a.addr = kAddr; a.wantsConnect = true; return a; }
}  // namespace

int main() {
    // Host mutate + markDirty + flush → client replica updates + onReplicated fires.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGame), cli(cliT, cliR, kGame);
        Replicator srvRep(srv, srvR), cliRep(cli, cliR);

        Scoreboard hostBoard, clientBoard;
        int notifyCount = 0;
        srvRep.replicate<Scoreboard>(kScoreId, &hostBoard);
        cliRep.replicate<Scoreboard>(kScoreId, &clientBoard, [&]{ ++notifyCount; });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));
        for (int i = 0; i < 4; ++i) { srv.poll(); cli.poll(); }

        hostBoard.scores = {10, 20};
        srvRep.markDirty(kScoreId);
        srvRep.flush();
        cli.poll();

        CHECK(clientBoard.scores.size() == 2u);
        CHECK(clientBoard.scores[0] == 10);
        CHECK(clientBoard.scores[1] == 20);
        CHECK(notifyCount == 1);

        // Coalescing: two markDirty in a frame → one flush → one sync/notify.
        hostBoard.scores = {1, 2, 3};
        srvRep.markDirty(kScoreId);
        srvRep.markDirty(kScoreId);
        srvRep.flush();
        cli.poll();
        CHECK(clientBoard.scores.size() == 3u);
        CHECK(notifyCount == 2);

        // No dirty → flush sends nothing → no extra notify.
        srvRep.flush();
        cli.poll();
        CHECK(notifyCount == 2);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --target test_replicator`
Expected: FAIL — `Replicator` / target missing.

- [ ] **Step 3: Write `engine/net/Replicator.h`**

```cpp
#pragma once

#include "core/Log.h"
#include "net/ByteStream.h"
#include "net/MessageRegistry.h"
#include "net/NetTransport.h"
#include "net/PeerManager.h"

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>

namespace iron {

// Game-assigned, stable, non-zero id for a replicated object.
using ReplicationId = std::uint32_t;

// Authoritative whole-object replication + client→server commands on top of
// PeerManager + MessageRegistry. The host owns authoritative objects, mutates
// them, marks them dirty, and flush()es (broadcast). Clients hold replicas that
// update on sync and fire an onReplicated (RepNotify) callback, and submit
// commands the host validates. All replication traffic rides one reserved tag
// (250), sub-dispatched internally.
//
// Lifetime: the referenced PeerManager and MessageRegistry MUST outlive the
// Replicator (it registers a raw handler capturing `this`; MessageRegistry has
// no unregister API).
class Replicator {
public:
    Replicator(PeerManager& peers, MessageRegistry& registry);
    ~Replicator();

    Replicator(const Replicator&) = delete;
    Replicator& operator=(const Replicator&) = delete;

    // Register a replicated object. Host: `obj` is the authoritative instance
    // (read on sync). Client: `obj` is the local replica (written on sync) and
    // `onReplicated` fires after each applied sync. Same call on both sides.
    template <typename T>
    void replicate(ReplicationId id, T* obj, std::function<void()> onReplicated = {}) {
        RepObject ro;
        ro.serialize   = [obj](ByteWriter& w) { serialize(w, *obj); };
        ro.deserialize = [obj](ByteReader& r) { deserialize(r, *obj); };
        ro.onReplicated = std::move(onReplicated);
        ro.dirty = false;
        objects_[id] = std::move(ro);
    }

    // Host: mark a replicated object changed; broadcast on the next flush().
    void markDirty(ReplicationId id);

    // Host: serialize + broadcast every dirty object (coalesced), clear dirty.
    // No-op on clients. Call once per tick AFTER peers.poll().
    void flush();

    // Host: send full current state of every replicated object to a freshly
    // joined peer. Call from the game's PeerManager onPeerJoined (peer != 0).
    void onPeerJoined(std::uint32_t peerId);

    // Host: register a validator/handler for a command type. The handler runs
    // only on the host, with the sender's peerId.
    template <typename Cmd>
    void onCommand(std::function<void(std::uint32_t, const Cmd&)> fn) {
        commandHandlers_[Cmd::kCmdId] =
            [fn = std::move(fn)](std::uint32_t fromPeer, ByteReader& r) {
                Cmd cmd{};
                deserialize(r, cmd);
                if (!r.failed()) fn(fromPeer, cmd);
            };
    }

    // Client: send a command to the host (reliable). No-op (warns) on the host.
    template <typename Cmd>
    void submitRequest(const Cmd& cmd) {
        if (peers_.isHost()) {
            Log::warn("Replicator: submitRequest called on host; ignored");
            return;
        }
        ByteWriter w;
        w.u8(kSubCommand);
        w.u32(Cmd::kCmdId);
        serialize(w, cmd);
        registry_.sendRaw(peers_.connectionFor(0), kReplicationTag,
                          std::span<const std::byte>(w.data().data(), w.data().size()),
                          SendReliability::Reliable);
    }

private:
    static constexpr std::uint8_t kReplicationTag = 250;
    static constexpr std::uint8_t kSubSync = 1;
    static constexpr std::uint8_t kSubCommand = 2;

    struct RepObject {
        std::function<void(ByteWriter&)> serialize;
        std::function<void(ByteReader&)> deserialize;
        std::function<void()> onReplicated;
        bool dirty = false;
    };

    void onPacket(ConnectionId conn, std::span<const std::byte> payload);
    void sendSyncTo(std::uint32_t peerId, ReplicationId id, RepObject& obj);

    PeerManager& peers_;
    MessageRegistry& registry_;
    std::unordered_map<ReplicationId, RepObject> objects_;
    std::unordered_map<std::uint32_t,
        std::function<void(std::uint32_t, ByteReader&)>> commandHandlers_;
};

}  // namespace iron
```

- [ ] **Step 4: Write `engine/net/Replicator.cpp`**

```cpp
#include "net/Replicator.h"

#include <vector>

namespace iron {

Replicator::Replicator(PeerManager& peers, MessageRegistry& registry)
    : peers_(peers), registry_(registry) {
    registry_.registerRawHandler(
        kReplicationTag,
        [this](ConnectionId c, std::span<const std::byte> p) { onPacket(c, p); });
}

Replicator::~Replicator() = default;

void Replicator::markDirty(ReplicationId id) {
    const auto it = objects_.find(id);
    if (it != objects_.end()) it->second.dirty = true;
}

void Replicator::sendSyncTo(std::uint32_t peerId, ReplicationId id, RepObject& obj) {
    const ConnectionId c = peers_.connectionFor(peerId);
    if (c == kInvalidConnection) return;
    ByteWriter w;
    w.u8(kSubSync);
    w.u32(id);
    obj.serialize(w);
    if (1 + w.data().size() > MessageRegistry::kMaxPayloadBytes) {
        Log::warn("Replicator: object %u serializes to %zu bytes, exceeds max; skipped",
                  static_cast<unsigned>(id), w.data().size());
        return;
    }
    registry_.sendRaw(c, kReplicationTag,
                      std::span<const std::byte>(w.data().data(), w.data().size()),
                      SendReliability::Reliable);
}

void Replicator::flush() {
    if (!peers_.isHost()) return;
    const std::vector<std::uint32_t> ids = peers_.peerIds();
    for (auto& [id, obj] : objects_) {
        if (!obj.dirty) continue;
        for (std::uint32_t pid : ids) sendSyncTo(pid, id, obj);
        obj.dirty = false;
    }
}

void Replicator::onPeerJoined(std::uint32_t peerId) {
    if (!peers_.isHost() || peerId == 0) return;
    for (auto& [id, obj] : objects_) sendSyncTo(peerId, id, obj);
}

void Replicator::onPacket(ConnectionId conn, std::span<const std::byte> payload) {
    ByteReader r(payload);
    const std::uint8_t sub = r.u8();
    if (r.failed()) return;

    if (sub == kSubSync) {
        const ReplicationId id = r.u32();
        const auto it = objects_.find(id);
        if (it == objects_.end()) return;
        it->second.deserialize(r);
        if (!r.failed() && it->second.onReplicated) it->second.onReplicated();
    } else if (sub == kSubCommand) {
        if (!peers_.isHost()) return;              // commands handled by host only
        const std::uint32_t cmdId = r.u32();
        if (r.failed()) return;
        const auto it = commandHandlers_.find(cmdId);
        if (it == commandHandlers_.end()) return;
        const auto fromPeer = peers_.peerIdFor(conn);
        if (!fromPeer) return;
        it->second(*fromPeer, r);
    }
}

}  // namespace iron
```

- [ ] **Step 5: Register in CMake**

`engine/CMakeLists.txt`, after `net/PeerManager.cpp`:
```cmake
  net/Replicator.cpp
```
`tests/CMakeLists.txt`, near the net tests:
```cmake
iron_add_test(test_replicator test_replicator.cpp)
```

- [ ] **Step 6: Build + run to verify it passes**

Run: `cmake -S . -B build-vk` then `cmake --build build-vk --target test_replicator` then `build-vk\tests\Debug\test_replicator.exe`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add engine/net/Replicator.h engine/net/Replicator.cpp tests/test_replicator.cpp engine/CMakeLists.txt tests/CMakeLists.txt
git commit -m "M64: Replicator object replication (replicate/markDirty/flush/sync + RepNotify) + tests"
```

---

## Task 4: Replicator — client→server commands

**Files:**
- Test: `tests/test_replicator.cpp` (append a case in `main`, before `return iron_test_result();`)
- (No production change — `onCommand`/`submitRequest`/command dispatch were written in Task 3's header/.cpp. This task verifies them.)

- [ ] **Step 1: Write the failing test** — append to `tests/test_replicator.cpp`. Add this command type to the file's anonymous namespace (top of file):

```cpp
// POD command (default-serialized). kCmdId is the game-assigned dispatch id.
struct AddScoreCmd { static constexpr std::uint32_t kCmdId = 1; int amount; };
```

Then append in `main`:
```cpp
    // Client submitRequest → host onCommand handler runs with sender peerId +
    // payload; host mutates authoritative state + flush → client sees result.
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGame), cli(cliT, cliR, kGame);
        Replicator srvRep(srv, srvR), cliRep(cli, cliR);

        Scoreboard hostBoard, clientBoard;
        srvRep.replicate<Scoreboard>(kScoreId, &hostBoard);
        cliRep.replicate<Scoreboard>(kScoreId, &clientBoard);

        std::uint32_t lastFromPeer = 999;
        srvRep.onCommand<AddScoreCmd>([&](std::uint32_t fromPeer, const AddScoreCmd& c) {
            lastFromPeer = fromPeer;
            if (c.amount <= 0) return;               // validation: reject non-positive (no-op)
            hostBoard.scores.push_back(c.amount);
            srvRep.markDirty(kScoreId);
        });

        CHECK(srv.start(hostArgs()));
        CHECK(cli.start(clientArgs()));
        for (int i = 0; i < 4; ++i) { srv.poll(); cli.poll(); }

        // Valid command.
        cliRep.submitRequest(AddScoreCmd{50});
        srv.poll();                 // host receives + handles command
        srvRep.flush();             // host broadcasts updated board
        cli.poll();                 // client applies sync
        CHECK(lastFromPeer == 1);   // client's peerId
        CHECK(hostBoard.scores.size() == 1u);
        CHECK(clientBoard.scores.size() == 1u);
        CHECK(clientBoard.scores[0] == 50);

        // Denied command (amount <= 0) → no state change → no sync.
        cliRep.submitRequest(AddScoreCmd{-5});
        srv.poll();
        srvRep.flush();
        cli.poll();
        CHECK(hostBoard.scores.size() == 1u);    // unchanged
        CHECK(clientBoard.scores.size() == 1u);
    }
```

- [ ] **Step 2: Run test to verify it fails (or passes if Task 3 is complete)**

Run: `cmake --build build-vk --target test_replicator` then run it.
Expected: PASS (Task 3 already implemented commands). If it FAILS, the failure pinpoints a command-path bug to fix in `Replicator` before proceeding — do not edit the test to pass.

- [ ] **Step 3: Commit**

```bash
git add tests/test_replicator.cpp
git commit -m "M64: test Replicator client->server commands (validate + apply + denied no-op)"
```

---

## Task 5: Replicator — late-join initial sync

**Files:**
- Test: `tests/test_replicator.cpp` (append a case in `main`)
- (Production `onPeerJoined` was written in Task 3; this verifies the late-join path end-to-end with a real second client joining after state exists.)

- [ ] **Step 1: Write the failing test** — append in `main`

```cpp
    // Late join: host has state before a client connects; on join the host pushes
    // full current state to just that peer (game wires onPeerJoined).
    {
        MockTransport srvT, cliT;
        MessageRegistry srvR(&srvT), cliR(&cliT);
        PeerManager srv(srvT, srvR, kGame), cli(cliT, cliR, kGame);
        Replicator srvRep(srv, srvR), cliRep(cli, cliR);

        // Host wires late-join sync into PeerManager's join callback.
        srv.setOnPeerJoined([&](std::uint32_t pid) { srvRep.onPeerJoined(pid); });

        Scoreboard hostBoard, clientBoard;
        srvRep.replicate<Scoreboard>(kScoreId, &hostBoard);
        cliRep.replicate<Scoreboard>(kScoreId, &clientBoard);

        // Host already has state BEFORE the client connects.
        hostBoard.scores = {7, 8, 9};
        srvRep.markDirty(kScoreId);

        CHECK(srv.start(hostArgs()));    // fires onPeerJoined(0) → onPeerJoined no-ops (peer 0)
        CHECK(cli.start(clientArgs()));
        // Pump: client connects → host onPeerJoined(1) → late-join sync sent.
        for (int i = 0; i < 6; ++i) { srv.poll(); cli.poll(); }

        CHECK(clientBoard.scores.size() == 3u);   // got current state on join
        CHECK(clientBoard.scores[2] == 9);
    }
```

- [ ] **Step 2: Run test to verify it passes**

Run: `cmake --build build-vk --target test_replicator` then run it.
Expected: PASS. If it fails, fix the `onPeerJoined`/`sendSyncTo` path in `Replicator` (not the test).

- [ ] **Step 3: Commit**

```bash
git add tests/test_replicator.cpp
git commit -m "M64: test Replicator late-join initial sync (full state to a joining peer)"
```

---

## Task 6: Smoke demo — `games/14-net-repl-demo`

**Files:**
- Create: `games/14-net-repl-demo/main.cpp`, `games/14-net-repl-demo/CMakeLists.txt`
- Modify: root `CMakeLists.txt`

A minimal two-instance proof over real GNS transport: a single replicated `int` counter. Press **Space** → `submitRequest(IncrementCmd{})`; host increments authoritative counter + `markDirty`; both windows show the same number (printed to log on change); a late-joining second client immediately gets the current value. Model the net setup + window loop on `games/05-net-cubes/main.cpp`.

- [ ] **Step 1: `games/14-net-repl-demo/CMakeLists.txt`**

```cmake
add_executable(net-repl-demo main.cpp)
target_link_libraries(net-repl-demo PRIVATE ironcore)
```
(No assets to copy.) In root `CMakeLists.txt`, after `add_subdirectory(games/13-loot)`:
```cmake
add_subdirectory(games/14-net-repl-demo)
```

- [ ] **Step 2: `games/14-net-repl-demo/main.cpp`**

Structure (fill from the net-cubes scaffold — window/app, `parseNetArgs`, the `peers.poll()`/render loop):

```cpp
// games/14-net-repl-demo/main.cpp — M64 smoke test: a replicated counter shared
// over the network. First instance hosts; later instances connect. Space sends
// an IncrementCmd to the host; the host owns the count and replicates it back.
#include "core/Application.h"
#include "core/Log.h"
#include "core/NetArgs.h"
#include "net/MessageRegistry.h"
#include "net/PeerManager.h"
#include "net/Replicator.h"
#include "net/backends/gns/GnsTransport.h"
#include <GLFW/glfw3.h>

namespace {
using namespace iron;
constexpr std::uint32_t kGameId = 0x6400C001u;
constexpr ReplicationId kCounterId = 1;
struct Counter { int value = 0; };
void serialize(ByteWriter& w, const Counter& c) { w.i32(c.value); }
void deserialize(ByteReader& r, Counter& c) { c.value = r.i32(); }
struct IncrementCmd { static constexpr std::uint32_t kCmdId = 1; };  // POD (empty)
}  // namespace

int main(int argc, char** argv) {
    iron::Application::Config cfg;
    cfg.title = "Iron Core - Net Repl Demo (M64)";
    cfg.width = 800; cfg.height = 200;
    iron::Application app(cfg);
    if (!app.valid()) { iron::Log::error("net-repl-demo: init failed"); return 1; }

    iron::GnsTransport transport;
    const iron::NetArgs netArgs = iron::parseNetArgs(argc, argv);
    iron::MessageRegistry registry(&transport);
    iron::PeerManager peers(transport, registry, kGameId);
    iron::Replicator repl(peers, registry);

    iron::Counter counter;
    repl.replicate<iron::Counter>(kCounterId, &counter, [&]{
        iron::Log::info("net-repl-demo: counter = %d", counter.value);
    });
    // Host validates + applies; clients' handler never runs (commands are host-only).
    repl.onCommand<IncrementCmd>([&](std::uint32_t fromPeer, const IncrementCmd&) {
        (void)fromPeer;
        counter.value += 1;
        repl.markDirty(kCounterId);
    });
    // Late-join: push current state to a new peer.
    peers.setOnPeerJoined([&](std::uint32_t pid) { repl.onPeerJoined(pid); });

    if (!peers.start(netArgs)) { iron::Log::error("net-repl-demo: net start failed"); return 1; }

    app.setUpdate([&](const iron::FrameTime&) {
        peers.poll();
        if (app.input().keyPressed(GLFW_KEY_SPACE)) {
            if (peers.isHost()) {                 // host can increment directly
                counter.value += 1; repl.markDirty(kCounterId);
            } else {
                repl.submitRequest(IncrementCmd{});
            }
        }
        repl.flush();                              // host broadcasts dirty (no-op on client)
    });
    app.setRender([&]{
        renderer_clear_only:;                       // no 3D content needed; clear via beginFrame/endFrame
    });

    app.run();
    return 0;
}
```
Notes for the implementer:
- Use the exact `Application`/render-loop idiom from `games/05-net-cubes/main.cpp` (it creates a renderer and calls `beginFrame`/`endFrame`; this demo has nothing to draw, so a bare clear is fine — copy net-cubes' minimal frame). Remove the `renderer_clear_only` placeholder; just do a `beginFrame(...)`/`endFrame()` with no draw calls, or skip rendering entirely if the app loop allows.
- `IncrementCmd` is an empty POD; `static constexpr std::uint32_t kCmdId` is required by `onCommand`/`submitRequest`. An empty struct is trivially copyable → default-serialized (0 bytes) fine.
- This is a **manual** smoke test (networking needs two processes); there's no unit test for the demo.

- [ ] **Step 3: Build + manual smoke**

Run: `cmake -S . -B build-vk` then `cmake --build build-vk --target net-repl-demo`
Expected: builds clean. Manual check (two terminals): run `net-repl-demo.exe` (host) and `net-repl-demo.exe --connect 127.0.0.1` (client); press Space in either → both windows log the same incrementing counter; start a third instance late → it logs the current value immediately.

- [ ] **Step 4: Commit**

```bash
git add games/14-net-repl-demo CMakeLists.txt
git commit -m "M64: net-repl-demo — replicated counter smoke test over GNS transport"
```

---

## Final verification (before PR)

- [ ] **Clean full build with exit-code check** (per `[[verify-clean-build-before-ci]]` — do NOT trust the tail or stale exes):

```bash
cmake --build build-vk > build.log 2>&1; echo "EXIT=$?"; grep -iE "error C[0-9]|error LNK|fatal error" build.log
```
Expected: `EXIT=0`, no error lines (only the pre-existing `fopen` C4996 warnings are acceptable).

- [ ] **Run the full test suite** — every `test_*.exe` in `build-vk\tests\Debug\`, especially `test_byte_stream`, `test_message_registry`, `test_replicator`. All print `OK - all checks passed`. Confirm the run happened AFTER the exit-0 build (no stale binaries).

- [ ] **PR** to `main`; background CI-watch; merge when green.

---

## Self-review notes (author)

- **Spec coverage:** ByteStream + POD default (T1) ✓; raw channel (T2) ✓; replicate/markDirty/flush/sync/RepNotify + coalescing (T3) ✓; commands + validation + denied no-op (T4) ✓; late-join (T5) ✓; reserved tag 250 + sub-dispatch ✓; reliable everywhere ✓; 1200-byte over-size guard (`sendSyncTo` warns + skips) ✓; smoke demo over GNS (T6) ✓; delta deferred (spec future-work) ✓.
- **Type/name consistency:** `ReplicationId`; `replicate<T>(id, T*, onReplicated)`; `markDirty(id)`; `flush()`; `onPeerJoined(peerId)`; `onCommand<Cmd>((peerId,Cmd))`; `submitRequest<Cmd>(cmd)`; sub-tags `kSubSync=1`/`kSubCommand=2`; `kReplicationTag=250`; command types need `static constexpr std::uint32_t kCmdId`; replicated/command types use ADL `serialize(ByteWriter&,const T&)`/`deserialize(ByteReader&,T&)` (POD via default). Consistent across tasks.
- **Ordering:** per-frame contract is `peers.poll()` → game update (submit/handle) → `repl.flush()`. Tests pump explicitly in that order.
- **Tasks 4 & 5** add no production code (the methods land in T3); they are verification tasks that lock the command + late-join behavior with dedicated cases. If either fails, the bug is in T3's `Replicator` and gets fixed there.
