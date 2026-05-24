# Networking Foundation Polish (M8.4) — Design

**Date:** 2026-05-24
**Track:** Networking — M8.4
**Status:** Approved — proceeding to implementation plan

## Motivation

M8.3 shipped the typed dispatcher and a working multiplayer game
(net-tag). Playtest surfaced honest sharp edges:

- Remote players snap-step at 30 Hz instead of gliding.
- net-cubes and net-tag accidentally interop (same Hello+Position
  wire format), confusing two-window playtests.
- No way to see network health in-game (the bugs we hit — late-joiner
  no state, broadcast loop frozen, GNS bind failures — would have
  been obvious with a small debug HUD).
- No clock common to client and server (needed for prediction +
  lag-compensation in M8.6+ shooter work).

This milestone is **foundation polish, not a new game**. The work
unblocks M8.5 (prediction + helpers) and M8.6 (hero shooter), and
makes the existing demos look + feel a generation better.

## Goals

- Generic `iron::TimeHistory<T>` engine helper — time-stamped sample
  buffer with interpolation query. Used client-side for smooth remote
  rendering (M8.4) and server-side for lag compensation (M8.6).
- Apply `TimeHistory` to net-cubes and net-tag for remote-player
  rendering. Local players still render at authoritative
  `player.position` directly.
- 4-byte game-id handshake: each game advertises a fixed ID on its
  HelloMsg; client rejects if mismatched. Prevents the cross-connect
  footgun.
- `iron::FixedTickScheduler` — accumulator-pattern fixed-timestep
  helper. Game gives it a tick rate (e.g. 30 Hz) and a callback;
  scheduler calls back N times per frame to advance the simulation
  in deterministic steps. Required for prediction (M8.5) but useful
  on its own (deterministic ScoreUpdate cadence in net-tag).
- `iron::Hud` gains a small network-stats widget that pulls live
  numbers (RTT, packet loss, in/out bandwidth, connection state)
  from `GnsTransport` and renders them with one call from the game.

## Non-goals

- **No prediction, reconciliation, or server-authoritative refactor.**
  Those are M8.5.
- **No PeerManager extraction or snapshot abstraction.** M8.5.
- **No hero shooter game.** M8.6.
- **No lag-compensation pattern in any current game.** The
  `TimeHistory<T>` primitive supports it; using it server-side is a
  M8.6 game-level task.
- **No new networked game.** M8.4 is engine + refactors only.
- **No new asset work.** Reuses existing CC0 textures + sunset
  skybox.
- **No Strandbound integration.** Strandbound stays single-player
  (per the 2026-05-24 direction shift: "moving on from Strandbound,
  building new improved games").

## Architecture

### File layout

```
engine/
├── net/
│   ├── TimeHistory.h             # NEW — generic time-stamped buffer with interpolation query
│   ├── NetworkStats.h + .cpp     # NEW — pulls GNS RealTimeStatus → simple POD struct
│   ├── GnsTransport.h            # MODIFY — add stats() accessor
│   └── GnsTransport.cpp          # MODIFY — implement stats() via GetConnectionRealTimeStatus
├── core/
│   └── FixedTickScheduler.h      # NEW — header-only accumulator-pattern helper
├── math/
│   └── Vec.h                     # MODIFY — add interpolate(Vec3, Vec3, float) free function
└── ui/
    └── Hud.h + .cpp              # MODIFY — add addNetworkStatsWidget convenience

games/
├── 05-net-cubes/
│   ├── Messages.h                # MODIFY — add static gameId byte
│   └── main.cpp                  # MODIFY — adopt TimeHistory + reject mismatched gameId + debug HUD
└── 06-net-tag/
    ├── Messages.h                # MODIFY — add static gameId byte
    └── main.cpp                  # MODIFY — same as above

tests/
├── test_time_history.cpp         # NEW — generic Vec3 interpolation tests
├── test_fixed_tick_scheduler.cpp # NEW — tick-count and dt-accumulator tests
└── CMakeLists.txt                # MODIFY — register two new tests

docs/engine/networking.md         # MODIFY — TimeHistory section, game-id handshake doc, debug HUD doc
```

### `iron::TimeHistory<T>`

```cpp
namespace iron {

// Time-stamped sample buffer with interpolation query. Two use cases:
//
//   Client-side (M8.4): sample(now - displayDelay) → smooth remote
//   player display tolerant of jitter and modest packet loss.
//
//   Server-side (M8.6+ shooter): sample(client.fireTimestamp) →
//   position to validate hit against (lag compensation).
//
// T must support free-function `interpolate(T a, T b, float t)`
// returning T at parameter t in [0, 1]. For iron::Vec3 we provide
// one in engine/math/Vec.h.
template <typename T>
class TimeHistory {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit TimeHistory(
        std::chrono::milliseconds retention = std::chrono::milliseconds{1000});

    // Push a new sample stamped with now().
    void push(const T& sample);
    void push(TimePoint at, const T& sample);  // testing

    // Sample the buffer at the given absolute time.
    // Returns nullopt if the buffer is empty.
    // If `at` is before earliest stored sample: returns earliest.
    // If `at` is after latest stored sample: returns latest (no extrapolation).
    // Otherwise: linearly interpolates between the two straddling samples.
    std::optional<T> sample(TimePoint at) const;

    // Convenience: sample at `now() - displayDelay`. Common client use.
    std::optional<T> sampleAtDelay(std::chrono::milliseconds displayDelay) const;

    std::size_t size() const;
    void clear();

private:
    std::deque<std::pair<TimePoint, T>> samples_;
    std::chrono::milliseconds retention_;
};

} // namespace iron
```

**Eviction:** on `push`, drop samples older than `now - retention`.
Default retention 1 second handles a few hundred ms of lag-comp
lookback comfortably.

**Default display delay** (for cubes/tag client-side use): **100 ms**.
This is the standard multiplayer compromise. Hardcoded in games; the
TimeHistory itself is delay-agnostic.

### Game-id handshake

Each game's `Messages.h` declares:
```cpp
namespace iron::netcubes {
constexpr std::uint32_t kGameId = 0x6E455442;  // "nEtB" ASCII; arbitrary marker
struct HelloMsg {
    static constexpr std::uint8_t kTag = 1;
    std::uint32_t gameId;   // = kGameId
    std::uint32_t peerId;
};
}
```

Host populates `gameId = kGameId` when sending Hello. Client's
`HelloMsg` handler:
```cpp
if (msg.gameId != iron::netcubes::kGameId) {
    iron::Log::error("net-cubes: connected to wrong game (gameId=0x%X)", msg.gameId);
    glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
    return;
}
```

**Tag/cubes use different gameIds** so cross-connect produces an
immediate clean error instead of partial mystery behaviour. ASCII
chosen for human-readability in logs: cubes = `'nEtB'`, tag =
`'tAGo'`.

### `iron::FixedTickScheduler`

Header-only helper:
```cpp
namespace iron {

// Accumulator-pattern fixed-timestep scheduler. Game owns one of these
// and calls `update(dt, [](){ /* one tick */ })` per frame. The
// scheduler invokes the callback zero-or-more times until the
// accumulated time has been consumed, guaranteeing deterministic
// tick spacing.
//
// Typical use:
//   iron::FixedTickScheduler ticker{std::chrono::milliseconds{33}};  // 30 Hz
//   while (running) {
//       float dt = frameDt();
//       ticker.update(dt, [&]() { simulationTick(); });
//       render();
//   }
class FixedTickScheduler {
public:
    explicit FixedTickScheduler(std::chrono::microseconds tickInterval);

    // Run `tickFn` once per accumulated tickInterval. May call zero
    // times (if frame was short) or several times in a row (if frame
    // was long after a stall). The caller's frame time dt is
    // accumulated; whole-tick worths are consumed and dispatched.
    template <typename TickFn>
    void update(float dtSeconds, TickFn&& tickFn);

    std::chrono::microseconds tickInterval() const;

private:
    std::chrono::microseconds tickInterval_;
    float accumulator_ = 0.0f;  // seconds
};

}
```

Why this matters now (without prediction): it lets the host run
gameplay logic at a fixed cadence regardless of render framerate.
net-tag's score broadcast is currently tied to `gameElapsedSec` —
fine but ad-hoc. Adopting `FixedTickScheduler` makes the cadence
explicit and lets us reuse the same primitive in M8.5/M8.6.

### Network stats + debug HUD

```cpp
namespace iron {

// One snapshot of a connection's network health. Pulled from
// GnsTransport's underlying GNS API once per frame.
struct ConnectionStats {
    float pingMs        = 0.0f;
    float packetLossPct = 0.0f;
    float jitterMs      = 0.0f;
    float bandwidthInKbps  = 0.0f;
    float bandwidthOutKbps = 0.0f;
    // Connection state as a short human-readable string ("Connected",
    // "Connecting", "ClosedByPeer", etc.) — useful for the HUD line.
    std::string state;
};

} // namespace iron

// GnsTransport gains:
//   ConnectionStats stats(ConnectionId conn) const;
//
// Implementation pulls SteamNetworkingQuickConnectionStatus
// via ISteamNetworkingSockets::GetConnectionRealTimeStatus.
```

**HUD convenience** in `iron::Hud`:
```cpp
// Register four pre-positioned text lines on the right side of the
// screen that the game can update each frame with the contents of a
// ConnectionStats struct. Returns a tag the game can pass to update.
struct NetStatsHudHandle {
    HudId pingId, lossId, bandwidthId, stateId;
};
NetStatsHudHandle Hud::addNetworkStatsWidget(Vec2 topRight,
                                              Vec4 color = {1,1,1,0.7f});

void Hud::updateNetworkStats(const NetStatsHudHandle& h,
                              const ConnectionStats& s);
```

Game wiring (3 lines):
```cpp
auto netHud = hud.addNetworkStatsWidget({1240, 12});
// each frame:
hud.updateNetworkStats(netHud, transport.stats(hostConn));
```

Top-right placement keeps it out of the way of the game-state HUD
in top-left.

### Refactors

**net-cubes (~30 lines diff):**
- `Messages.h` gets `kGameId = 'nEtB'` + `HelloMsg.gameId` field.
- `main.cpp`: client rejects mismatched gameId on Hello receipt.
- Replace `struct CubeState { Vec3 displayed, target; }` and the
  per-frame lerp with `std::unordered_map<uint32_t, TimeHistory<Vec3>>`.
  Render at `history.sampleAtDelay(100ms)` for remote peers; local
  cube renders at `player.position` unchanged.
- Add 3-line network-stats HUD widget for the client.

**net-tag (~35 lines diff):**
- Same gameId + HelloMsg changes.
- Same TimeHistory refactor.
- Wrap the score-broadcast cadence in `FixedTickScheduler` (1 Hz
  ticks) — replaces the current `gameElapsedSec - lastScoreBroadcastSec`
  bookkeeping.
- Same network-stats HUD widget.

## Testing

`tests/test_time_history.cpp` (Vec3, no real time needed; uses
explicit TimePoints throughout):
1. Empty buffer → `sample(any)` returns nullopt.
2. Single sample at t=1s → `sample(t=1s)` returns it; `sample(t=2s)`
   also returns it (no extrapolation).
3. Two samples at t=1s (Vec3{0,0,0}) and t=2s (Vec3{10,0,0}) →
   `sample(t=1.5s)` returns Vec3{5,0,0}.
4. Query before earliest → returns earliest.
5. Query after latest → returns latest (no extrapolation, no
   undefined behavior).
6. Push 10 samples spaced 200ms apart with 1s retention; oldest 5
   should be evicted (or close to it depending on push semantics).

`tests/test_fixed_tick_scheduler.cpp`:
1. 30Hz scheduler, dt=0.1s → exactly 3 callback invocations (3 ticks
   per 100ms).
2. 30Hz scheduler called 4 times with dt=0.01s → 1 invocation total
   (0.04s accumulated < 0.033s tick interval × 2; only 1 tick
   consumed).
3. 30Hz, dt=2.0s → 60 invocations in one update call (catches up).
4. Accumulator preserves remainder across calls.

Both tests are deterministic, real-time-free.

## Files

| Path | Action |
|------|--------|
| `engine/net/TimeHistory.h` | new (header-only template) |
| `engine/net/NetworkStats.h` | new |
| `engine/net/NetworkStats.cpp` | new (empty or only out-of-line virtual stub if any; mostly the struct) |
| `engine/net/GnsTransport.h` | modify — `ConnectionStats stats(ConnectionId) const` |
| `engine/net/GnsTransport.cpp` | modify — implement via `GetConnectionRealTimeStatus` |
| `engine/core/FixedTickScheduler.h` | new (header-only) |
| `engine/math/Vec.h` | modify — add `interpolate(Vec3, Vec3, float)` |
| `engine/ui/Hud.h` | modify — `NetStatsHudHandle`, `addNetworkStatsWidget`, `updateNetworkStats` |
| `engine/ui/Hud.cpp` | modify — implement the two methods |
| `tests/test_time_history.cpp` | new |
| `tests/test_fixed_tick_scheduler.cpp` | new |
| `tests/CMakeLists.txt` | modify — register two new tests |
| `games/05-net-cubes/Messages.h` | modify — `kGameId`, `HelloMsg.gameId` |
| `games/05-net-cubes/main.cpp` | modify — gameId reject, TimeHistory refactor, net-stats HUD |
| `games/06-net-tag/Messages.h` | modify — same |
| `games/06-net-tag/main.cpp` | modify — same + adopt FixedTickScheduler for score cadence |
| `docs/engine/networking.md` | modify — TimeHistory + game-id + debug HUD sections |

## Risks

- **100 ms display delay for remote players** — tag detection on
  the host uses authoritative positions (the latest known), so
  delayed display does NOT make tagging harder on the host. On the
  client, the displayed "it" cube is 100 ms behind reality, but
  client doesn't run tag detection — only renders. Game feel: same
  as a standard multiplayer game. Acceptable.
- **First sample for a new peer means brief invisibility** — a peer
  appears on first Position message (history has 1 sample → renders
  at that sample). Before that, history is empty → render skipped.
  Same edge case as today's "wait for first message" path.
- **`GetConnectionRealTimeStatus` availability** — older GNS
  versions had different APIs. The vcpkg-pinned version (~Apr 2024
  GNS) has this. Documented in build-setup if anyone bumps the
  baseline.
- **Game-id collision** — if two games pick the same 4-byte ID, the
  cross-connect bug returns. Mitigation: tabulate gameIds in
  `docs/engine/networking.md` (a registry of one-line entries).

## Out of scope (M8.5+)

- `iron::PeerManager` (extract repeated peerId/conn/Hello flow) — M8.5
- `iron::PredictionEngine<TInput, TState>` — M8.5
- Server-authoritative refactor of net-tag — M8.5
- Snapshot pattern documentation + reference impl — M8.5
- Lag-compensation pattern documentation + game-side impl — M8.6
  (alongside hero shooter)
- Hero shooter game itself — M8.6
- NAT traversal, Steam Datagram Relay — later
- Strandbound multiplayer — explicitly off-roadmap per 2026-05-24
  direction shift
