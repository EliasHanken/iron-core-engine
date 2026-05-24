# Typed Message Dispatcher + Tag Game (M8.3) — Design

**Date:** 2026-05-24
**Track:** Networking — M8.3
**Status:** Approved — proceeding to implementation plan

## Motivation

M8.2 shipped a runnable multiplayer demo (`games/05-net-cubes`) on the
`iron::NetTransport` wrapper. Game code there hand-rolls protocol
plumbing: a 1-byte tag prefix, a manual `parse(bytes)` returning a
tagged union, an inline switch on tag, raw `memcpy` per field. Every
future networked game would duplicate that pattern.

This milestone closes that gap with two deliverables in one PR:

1. **Engine: `iron::MessageRegistry`** — a typed-message dispatch layer
   on top of `NetTransport`. Game code defines POD message structs with
   a `static constexpr std::uint8_t kTag` and registers handlers via
   `registry.registerHandler<MsgType>(...)`. The engine handles
   serialisation, framing, dispatch, type safety.
2. **`games/06-net-tag`** — a brand-new N-player tag game that exercises
   the dispatcher with multiple message types and real gameplay state
   (round timer, "it" handoff via cube collision, scoring).

Both `net-cubes` and `net-tag` gain LAN play via a shared
`--connect <ip>` / `--port <n>` CLI parser in `engine/core/NetArgs`.

The cubes demo's hand-rolled protocol gets deleted in the process —
both demos end up on the same typed-dispatcher API, proving the
abstraction works for the existing scenario and the new one.

## Goals

- Game code declares messages as POD structs with `static constexpr
  std::uint8_t kTag`, never touches raw bytes or tag switches.
- `registry.send<Msg>(conn, msg, reliability)` and
  `registry.registerHandler<Msg>([&](conn, msg) { ... })` are the only
  send/receive APIs game code needs for typed traffic.
- LAN play between two physical machines: client invokes
  `net-tag.exe --connect <host-ip>`; host invokes `net-tag.exe`.
- N-player tag with 60-second rounds, live leaderboard, "it" handoff on
  cube collision (< 1.5m + 0.5s cooldown).
- `games/05-net-cubes` ships its main on top of the new dispatcher with
  the old `Protocol.{h,cpp}` and its unit test deleted.

## Non-goals

- **No snapshot interpolation tuning.** The cubes' existing simple
  lerp-toward-target is reused as-is for net-tag; no further smoothing
  work this milestone.
- **No client-side prediction / lag compensation.** The host is
  authoritative; clients are passive renderers of host state.
- **No replication of arbitrary state** ("network variables" /
  property replication). M8.3 adds typed point-to-point messages, not a
  property sync system.
- **No reconnect or host migration.** Host quits → clients exit cleanly
  (already true in cubes; same in tag).
- **No NAT traversal / Steam Datagram Relay / matchmaking.** LAN only.
- **No variable-length messages** (no strings, no vectors inside
  message structs). POD-only. Length-prefixed string helper is a future
  milestone.
- **No input rebinding** — both demos stay hard-coded WASD + chase camera.
- **No game integration beyond the new demo.** Strandbound stays
  single-player.

## Architecture

### File layout

```
engine/
├── core/
│   └── NetArgs.h + .cpp        # parseNetArgs(argc, argv) → { addr, wantsConnect }
└── net/
    └── MessageRegistry.h + .cpp # typed dispatch layer over NetTransport

games/
├── 05-net-cubes/
│   ├── Messages.h              # NEW — kTag-style POD message structs
│   ├── Protocol.h              # DELETED
│   ├── Protocol.cpp            # DELETED
│   └── main.cpp                # MODIFIED — uses MessageRegistry + parseNetArgs
└── 06-net-tag/                 # NEW
    ├── CMakeLists.txt
    ├── Messages.h              # 6 typed messages (Hello, Position, TagSwap, ScoreUpdate, RoundStart, RoundEnd)
    └── main.cpp                # host-authoritative tag with scoring

tests/
├── test_message_registry.cpp   # NEW — round-trip via MockTransport
└── test_net_cubes_protocol.cpp # DELETED (registry tests cover the same ground)
```

### The dispatcher API

```cpp
namespace iron {

// Typed message dispatch on top of NetTransport. Wraps a single
// transport instance; installs its own onMessage handler at construction.
// Game code must not call `transport->setOnMessage(...)` after wrapping.
//
// Wire format: [u8 tag][raw memcpy of msg]
//
// Message type requirements (enforced by static_assert):
//   - std::is_trivially_copyable_v<Msg> (POD only)
//   - Msg::kTag is a static constexpr std::uint8_t
//   - sizeof(Msg) + 1 <= kMaxPayloadBytes (1200, conservative MTU)
//
// Tag 0 is reserved (treated as "unknown / invalid"). Tag namespace is
// per-registry; different games can reuse the same tag values without
// conflict because each MessageRegistry has its own handler table.
class MessageRegistry {
public:
    explicit MessageRegistry(NetTransport* transport);

    template <typename Msg>
    void registerHandler(std::function<void(ConnectionId, const Msg&)> fn);

    template <typename Msg>
    bool send(ConnectionId conn, const Msg& msg, SendReliability reliability);

    // Convenience: send to a list of connections in one call.
    template <typename Msg>
    void sendToAll(std::span<const ConnectionId> conns,
                   const Msg& msg, SendReliability reliability);

private:
    NetTransport* transport_;
    std::unordered_map<std::uint8_t,
        std::function<void(ConnectionId, std::span<const std::byte>)>> handlers_;
};

} // namespace iron
```

**Serialisation:** `send<Msg>` allocates `sizeof(Msg) + 1` bytes on the
stack (small fixed-size buffer), writes the tag, memcpy's the struct,
and calls `transport_->send(conn, span, reliability)`.

**Dispatch:** `MessageRegistry`'s ctor calls
`transport->setOnMessage([this](...){ this->dispatch(...); })`.
`dispatch` reads byte 0 as the tag, looks up the handler, calls it with
a span over bytes [1..end]. The typed handler stub memcpy's the bytes
back into a stack `Msg` and invokes the user callback.

**Error handling (silent drops with warnings):**
- Tag 0 → drop with warning
- Tag with no registered handler → drop with warning
- Payload size doesn't match `sizeof(Msg)` → drop with warning, do NOT
  attempt the memcpy

### CLI parser

```cpp
namespace iron {

struct NetArgs {
    NetAddress addr{0x7F000001, 30005};  // default: 127.0.0.1:30005
    bool wantsConnect = false;            // false → listen; true → connect to addr
};

// Parses `--connect <ip>` and `--port <n>` from argv. Returns sensible
// defaults if neither is present (host on 127.0.0.1:30005). Unknown
// args are silently ignored — games may layer their own parsing.
NetArgs parseNetArgs(int argc, char** argv);

} // namespace iron
```

Both demos call this; auto-detect-host-vs-client logic stays the same
(`wantsConnect ? connect : listen → fall back to connect`). When
`wantsConnect` is true, we skip the listen attempt entirely and go
straight to `connect()`.

### The tag game

**Star topology, host-authoritative:**
- Host runs the simulation: detects tags, owns the round timer, owns
  the score table, broadcasts state changes.
- Clients send position; receive everything else.

**Game state on host:**
```cpp
struct PlayerInfo {
    iron::Vec3 position;
    float itTimeAccumSec = 0.0f;
    float lastTaggedTimeSec = -1.0f;  // for the 0.5s post-swap cooldown
};
std::unordered_map<std::uint32_t /*peerId*/, PlayerInfo> players_;

std::uint32_t itPeerId_ = 0;
float roundTimeRemainingSec_ = 60.0f;
float lastScoreBroadcastSec_ = 0.0f;
bool roundActive_ = false;
float roundEndDisplayUntilSec_ = 0.0f;
```

**Per-tick on host:**
1. `transport.poll()` (registry callbacks fire — PositionMsg updates `players_[id].position`)
2. If round active:
   - `roundTimeRemainingSec_ -= dt`
   - `players_[itPeerId_].itTimeAccumSec += dt`
   - Tag check: for every non-"it" player, if `distance(it.pos, other.pos) < 1.5` AND `now - lastTaggedTimeSec > 0.5` → swap "it" to that player, set their `lastTaggedTimeSec = now`, broadcast `TagSwapMsg`
   - Every 1 second of game time: broadcast one `ScoreUpdateMsg` per player
   - If `roundTimeRemainingSec_ <= 0`: pick winner (lowest `itTimeAccumSec`), broadcast `RoundEndMsg`, set `roundActive_ = false`, schedule round-end display
3. If not round active and `now > roundEndDisplayUntilSec_` (5 second
   round-end display window):
   - Reset scores, pick random initial "it", broadcast `RoundStartMsg`,
     set `roundActive_ = true`, `roundTimeRemainingSec_ = 60.0f`

If a client joins mid-round (host's `onConnectionOpened` fires), the
host sends them a `HelloMsg` (peer-id assignment, as today) and a
`RoundStartMsg` snapshot with the **remaining** round time so the late
joiner sees the same clock. They start with `itTimeAccumSec = 0` —
late joiners have a tiny advantage; acceptable for a casual demo.

**Visual:**
- Same scene/material setup as cubes (CC0 ground, sunset skybox, chase
  camera on the local player)
- "It" cube renders with red emissive boost: `emissive = base + Vec3{1.5, 0, 0}`
- HUD top-left:
  - `You are IT!` (red) or `Run!` (white)
  - `Round: 0:42` (countdown)
  - Leaderboard sorted ascending by `itTimeSec`: `peer 0: 12.4s`, `peer 1: 7.2s`, ...
  - During round-end display: `Winner: peer N!`

**Client logic:**
- Standard host-detection (listen or connect per CLI)
- Sends `PositionMsg` ~30Hz unreliable
- Receives `HelloMsg` → assigns `myPeerId`
- Receives `PositionMsg` from peers (host rebroadcasts) → update remote cubes (with lerp interp from cubes)
- Receives `TagSwapMsg` → update local `itPeerId` (HUD + emissive)
- Receives `ScoreUpdateMsg` → update leaderboard
- Receives `RoundStartMsg` / `RoundEndMsg` → switch HUD state

### The cubes refactor

`games/05-net-cubes/main.cpp` swap: where it currently builds `sendBuf`
via `writePosition(...)` then calls `transport.send(...)`, it now calls
`registry.send<PositionMsg>(c, PositionMsg{myId, x, y, z}, Unreliable)`.
Where it currently has the inline `switch (parsed->tag)`, it now
registers two `registerHandler<...>` calls during setup.

`games/05-net-cubes/Messages.h` (NEW) replaces `Protocol.h`:
```cpp
namespace iron::netcubes {
struct HelloMsg    { static constexpr std::uint8_t kTag = 1; std::uint32_t peerId; };
struct PositionMsg { static constexpr std::uint8_t kTag = 2; std::uint32_t peerId; float x, y, z; };
}
```

`Protocol.h`, `Protocol.cpp`, `tests/test_net_cubes_protocol.cpp` —
deleted. The registry's tests cover the same correctness ground.

## Testing

`tests/test_message_registry.cpp` — runs against a pair of
`MockTransport`s. Covers:

1. **Round-trip per type.** Define 2-3 test message structs with
   different tags + sizes. Register handlers on B. From A, send each
   type. After B.poll, verify each handler was called once with the
   right contents.
2. **Wrong-size payload silently dropped.** Manually send a buffer
   with a known tag but wrong size via the raw transport. Verify the
   handler was NOT called and no crash.
3. **Unregistered tag silently dropped.** Send a typed message whose
   tag has no handler. Verify nothing called, no crash.
4. **Multiple messages in one poll.** Send 3 messages, poll once,
   verify all 3 handlers fire in order.
5. **Send-on-invalid-connection returns false.** Pass `kInvalidConnection`
   to `registry.send<Msg>(...)`, verify it returns false and nothing was
   queued.

No new unit test for net-tag (game integration; same policy as cubes).
The new shared `parseNetArgs` gets unit-test coverage via
`tests/test_net_args.cpp` (covers default, `--connect`, `--port`, both
together, malformed inputs).

## Files

| Path | Action |
|------|--------|
| `engine/net/MessageRegistry.h` + `.cpp` | new |
| `engine/core/NetArgs.h` + `.cpp` | new |
| `tests/test_message_registry.cpp` | new |
| `tests/test_net_args.cpp` | new |
| `games/05-net-cubes/Messages.h` | new |
| `games/05-net-cubes/Protocol.h` | **delete** |
| `games/05-net-cubes/Protocol.cpp` | **delete** |
| `tests/test_net_cubes_protocol.cpp` | **delete** |
| `games/05-net-cubes/main.cpp` | modify — use MessageRegistry + parseNetArgs |
| `games/05-net-cubes/CMakeLists.txt` | modify — drop Protocol.cpp source |
| `games/06-net-tag/CMakeLists.txt` | new |
| `games/06-net-tag/main.cpp` | new |
| `games/06-net-tag/Messages.h` | new |
| `tests/CMakeLists.txt` | modify — swap protocol-test for registry-test + net-args-test |
| `engine/CMakeLists.txt` | modify — add `core/NetArgs.cpp` and `net/MessageRegistry.cpp` to ironcore |
| top-level `CMakeLists.txt` | modify — `add_subdirectory(games/06-net-tag)` |
| `docs/engine/networking.md` | modify — typed-dispatch section + new-game section |

## Risks

- **POD-only constraint.** Future messages with dynamic data (chat,
  level names) won't fit. Acceptable; we add a length-prefixed
  string-message helper as a separate later type.
- **256 tag types per registry.** Hard limit. Fine for our scale.
- **Endianness assumption** still host-byte-order, Windows-only. Same
  as the cubes had.
- **Tag collision within a single game.** Two structs with the same
  `kTag` value: the second `registerHandler` silently replaces the
  first. Mitigation: each `Messages.h` keeps all tags grouped in one
  file so collisions are obvious during code review.
- **N-player fan-out scaling.** Host fans every position to every
  other client. At 30Hz × 8 players = 1680 msgs/sec out of host.
  Trivial on LAN; not a concern for our scale.
- **LAN firewall.** Windows Firewall may prompt the first time
  `net-tag.exe` binds. Standard "Allow" dialog; documented in
  `docs/engine/networking.md`.

## Out of scope (M8.4+)

- Snapshot interpolation tuning beyond what cubes already does
- Client-side prediction / lag compensation
- Property replication / "network variables"
- Reconnect / host migration / mid-session join
- NAT traversal, Steam Datagram Relay, matchmaking
- Length-prefixed string/array messages
- Strandbound multiplayer integration
- Input rebinding / config files
