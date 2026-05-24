# Prediction Stack + Helpers (M8.5) — Design

**Date:** 2026-05-24
**Track:** Networking — M8.5
**Status:** Approved — proceeding to implementation plan

## Motivation

M8.4 shipped foundation primitives (`TimeHistory<T>`, `FixedTickScheduler`,
`ConnectionStats`, debug HUD). What's still copy-pasted in every
networked game today: ~150 lines of peer-bookkeeping boilerplate
(connToPeerId map, peerId assignment, Hello flow, gameId validation,
disconnect cleanup) plus zero engine support for the prediction loop
that the upcoming M8.6 hero shooter will need (server-authoritative
simulation, client-side input prediction, reconciliation against
authoritative state).

M8.5 closes both gaps with engine helpers:

- **`iron::PeerManager`** owns peer lifecycle, Hello handshake, gameId
  validation, and the conn↔peerId map. Reserves message **tag=1** for
  its `peer::HelloMsg` so games start their tags at 2.
- **`iron::PredictionEngine<TInput, TState>`** maintains client-side
  input history and reconciles against authoritative server state.

Both games refactor to use `PeerManager` (cleanup, no behavior change).
Net-tag additionally refactors to **server-authoritative position**
using `PredictionEngine` — the first reference implementation of the
prediction pattern in the engine, validating the abstraction before the
hero shooter consumes it.

Net-cubes intentionally stays client-authoritative as the simpler
reference pattern for game devs to copy alongside net-tag's full
prediction stack.

## Goals

- `iron::PeerManager` owns Hello + peer lifecycle. Game-level
  boilerplate for a networked game drops to ~5 lines.
- `iron::PredictionEngine<TInput, TState>` implements input-history +
  reconcile-or-replay against an authoritative state.
- Both `games/05-net-cubes` and `games/06-net-tag` refactor onto
  `PeerManager` (deleting their hand-rolled HelloMsg flow).
- `games/06-net-tag` additionally adopts `PredictionEngine` + a
  server-authoritative position model: client sends `PlayerInputMsg`,
  host applies inputs, broadcasts `AuthorityPositionMsg`. Client uses
  `PredictionEngine` for local-player responsiveness.
- Snapshot pattern documented in `docs/engine/networking.md` with
  net-tag's existing late-joiner snapshot (RoundStartMsg + ScoreUpdate
  flush in `onPeerJoined`) as the reference. No engine abstraction.

## Non-goals

- **No `SnapshotBroadcaster<T>` abstraction.** Pattern documented;
  helper would be premature with only one example. Add when 2+ games
  ask for it.
- **No lag-compensation game-side impl.** Engine has `TimeHistory<T>`
  ready; net-tag doesn't need it (no hit detection). M8.6 hero shooter
  uses it server-side for hit validation.
- **No prediction tolerance / float-comparison helper.** `TState`'s
  `operator==` is used; works for deterministic same-platform sims.
  Add comparator parameter if it ever bites.
- **No reconnect or host migration.** Disconnect = game-side cleanup
  + (for clients) graceful exit. Same as today.
- **No PeerManager refactor for net-cubes that changes its model.** Net-cubes
  keeps client-authoritative position — only Hello/peer bookkeeping
  moves to PeerManager.
- **No new game.** Hero shooter is M8.6.
- **No tag re-numbering across the registry.** Game tags shift up by 1
  in net-tag (because the engine reserves tag=1 now and we delete
  net-tag's own HelloMsg=1; PositionMsg moves from 2→2 stays, but new
  PlayerInputMsg gets tag=2 — see wire format below). This is a
  breaking wire change, accepted because we control both ends.

## Architecture

### File layout

```
engine/net/
├── PeerMessages.h           # NEW — peer::HelloMsg (kTag = 1 reserved)
├── PeerManager.h + .cpp     # NEW — owns Hello + lifecycle + conn↔peerId map
└── PredictionEngine.h       # NEW — header-only template

tests/
├── test_peer_manager.cpp    # NEW — paired MockTransports + PeerManagers
└── test_prediction_engine.cpp # NEW — applyInput/reconcile/match/mismatch/reset

games/05-net-cubes/
├── Messages.h               # MODIFY — DELETE HelloMsg (PeerManager owns)
└── main.cpp                 # MODIFY — adopt PeerManager (~80 lines deleted)

games/06-net-tag/
├── Messages.h               # MODIFY — DELETE HelloMsg; add PlayerInputMsg + AuthorityPositionMsg; renumber TagSwap+
└── main.cpp                 # MODIFY — PeerManager + PredictionEngine + server-authoritative position

engine/CMakeLists.txt        # MODIFY — add net/PeerManager.cpp
tests/CMakeLists.txt         # MODIFY — register two new tests
docs/engine/networking.md    # MODIFY — PeerManager + PredictionEngine + Snapshot pattern sections
```

### `iron::PeerManager`

```cpp
namespace iron {

namespace peer {
// Reserved tag=1. Games' MessageRegistry tags must start at 2.
struct HelloMsg {
    static constexpr std::uint8_t kTag = 1;
    std::uint32_t gameId;
    std::uint32_t peerId;
};
}

class PeerManager {
public:
    using PeerJoinedFn = std::function<void(std::uint32_t peerId)>;
    using PeerLeftFn   = std::function<void(std::uint32_t peerId)>;

    // Constructs the manager. Installs the HelloMsg handler on the
    // registry — game must NOT register its own handler for tag=1.
    PeerManager(NetTransport& transport, MessageRegistry& registry,
                std::uint32_t gameId);
    ~PeerManager();

    // Non-copyable, non-movable (holds raw pointers + transport callbacks).
    PeerManager(const PeerManager&) = delete;
    PeerManager& operator=(const PeerManager&) = delete;

    // Open the transport per NetArgs. Returns false on failure (transport.start
    // failed, neither listen nor connect succeeded).
    bool start(const NetArgs& args);
    void stop();

    bool isHost() const;
    std::uint32_t myPeerId() const;       // 0 if client pre-Hello
    bool hasIdentity() const;              // host true after start; client true after Hello

    // Iterate connected peers (host: all clients; client: just host=peerId 0).
    std::vector<std::uint32_t> peerIds() const;
    ConnectionId connectionFor(std::uint32_t peerId) const;
    std::optional<std::uint32_t> peerIdFor(ConnectionId conn) const;

    void setOnPeerJoined(PeerJoinedFn);
    void setOnPeerLeft(PeerLeftFn);

    // Drive: pump the transport. Game calls each frame.
    void poll();

    // Send a typed message to one peer by peerId. Returns false if
    // the peerId is not connected.
    template <typename Msg>
    bool send(std::uint32_t peerId, const Msg& msg, SendReliability r);

    // Host-only: broadcast to all connected clients (does NOT loop
    // back to the host itself).
    template <typename Msg>
    void broadcastToAll(const Msg& msg, SendReliability r);

private:
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
    PeerLeftFn onLeft_;

    // Whether we should treat the local node as having "joined" peerId 0
    // ourselves. The host fires onJoined(0) immediately on start so game
    // code initializing per-peer state has a uniform path.
};

}
```

**Internal lifecycle:**
- **Ctor:** registers PeerManager's own HelloMsg handler on the registry; installs `setOnConnectionOpened`/`setOnConnectionClosed` on the transport.
- **`start(NetArgs)`:** calls `transport.start()`; if `args.wantsConnect`, calls `transport.connect()` and stores `hostConn_`; else calls `transport.listen()` and sets `isHost_ = true`. If host, fires `onPeerJoined(0)` immediately so game's per-peer init path is uniform.
- **`onConnectionOpened(c)`:** if host, assigns a new peerId, stores in maps, sends `peer::HelloMsg{gameId_, assigned}` reliably. If client, no action until HelloMsg arrives.
- **Hello handler:** if not host and `msg.gameId == gameId_`, sets `myPeerId_ = msg.peerId`, fires `onPeerJoined(myPeerId_)` then `onPeerJoined(0)` (the host) for symmetry. If `msg.gameId != gameId_`, logs an error and closes the connection (game's main loop sees the disconnect and exits naturally).
- **`onConnectionClosed(c)`:** look up peerId, fire `onPeerLeft(peerId)`, remove from maps. On client, also calls `glfwSetWindowShouldClose`-equivalent... actually the engine doesn't depend on GLFW, so client just fires `onPeerLeft(0)` and the game's onPeerLeft callback can choose to exit.
- **`poll()`:** delegates to `transport.poll()`.

### `iron::PredictionEngine<TInput, TState>`

```cpp
namespace iron {

template <typename TInput, typename TState>
class PredictionEngine {
public:
    using SimulateFn =
        std::function<TState(const TState&, const TInput&, float dtSec)>;

    PredictionEngine(SimulateFn simulate, float fixedDtSec,
                     TState initial = {});

    // Apply input, advance predicted state, record in history.
    // Returns the new inputId (monotonic; starts at 1).
    std::uint32_t applyInput(const TInput& input);

    // Latest locally-predicted state.
    const TState& predictedState() const;

    // Reconcile against authoritative server state for the input
    // identified by lastConfirmedInputId.
    //   - Drop history entries with inputId <= lastConfirmedInputId.
    //   - If the prediction at lastConfirmedInputId matched authState,
    //     leave predictedState alone.
    //   - If mismatched (or we no longer have that input in history),
    //     snap predictedState to authState and replay all surviving
    //     history entries to bring it forward to "now".
    void reconcile(const TState& authState,
                   std::uint32_t lastConfirmedInputId);

    std::size_t historySize() const;

    // Wipe history; set the predicted state.
    void reset(const TState& state);

private:
    struct Entry { std::uint32_t inputId; TInput input; TState predicted; };

    SimulateFn simulate_;
    float fixedDt_;
    TState predicted_;
    std::uint32_t nextInputId_ = 1;
    std::deque<Entry> history_;
};

}
```

Comparison uses `TState::operator==` (exact). Acceptable because client
and server run the same deterministic simulation on the same inputs in
the same order on the same platform.

### Server-authoritative net-tag refactor

**Wire-format changes (deletes + adds + renumbers):**

| Message | M8.4 tag | M8.5 tag | Note |
|---------|----------|----------|------|
| `peer::HelloMsg` | — | 1 | Owned by PeerManager; tag 1 reserved |
| `nettag::HelloMsg` | 1 | — | DELETED |
| `nettag::PlayerInputMsg` | — | 2 | NEW: client → host each input frame |
| `nettag::AuthorityPositionMsg` | — | 3 | NEW: host → all, broadcasts authoritative position |
| `nettag::PositionMsg` | 2 | — | DELETED (replaced by AuthorityPositionMsg above) |
| `nettag::TagSwapMsg` | 3 | 4 | renumbered (was 3) |
| `nettag::ScoreUpdateMsg` | 4 | 5 | renumbered |
| `nettag::RoundStartMsg` | 5 | 6 | renumbered |
| `nettag::RoundEndMsg` | 6 | 7 | renumbered |

```cpp
namespace iron::nettag {
constexpr std::uint32_t kGameId = 0x7441476Fu;  // unchanged

struct PlayerInputMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t inputId;
    float dx, dy, dz;    // movement delta in world coords for this tick
};

struct AuthorityPositionMsg {
    static constexpr std::uint8_t kTag = 3;
    std::uint32_t peerId;
    float x, y, z;
    std::uint32_t lastInputId;  // 0 for host's own peer (no client to reconcile)
};

struct TagSwapMsg     { static constexpr std::uint8_t kTag = 4; std::uint32_t newItPeerId; };
struct ScoreUpdateMsg { static constexpr std::uint8_t kTag = 5; std::uint32_t peerId; float itTimeSec; };
struct RoundStartMsg  { static constexpr std::uint8_t kTag = 6; std::uint32_t initialItPeerId; float roundDurationSec; };
struct RoundEndMsg    { static constexpr std::uint8_t kTag = 7; std::uint32_t winnerPeerId; };
}
```

**Net-tag main.cpp flow change:**

A local `simulate` function computes the next state from previous state + input:

```cpp
struct PlayerState { float x, y, z; };
struct PlayerInput { float dx, dy, dz; };

auto simulate = [](const PlayerState& s, const PlayerInput& i, float /*dt*/) {
    return PlayerState{s.x + i.dx, s.y + i.dy, s.z + i.dz};
};
```

(The simulation here is trivial pure-addition — server runs the SAME
function so prediction always matches. The machinery is what matters
for M8.6.)

**Local player flow (every input frame):**
1. Compute `PlayerInput{dx, dy, dz}` from current input + WASD/QE/yaw.
   This is done at the per-tick rate of a `FixedTickScheduler{1/30s}`
   so input is delta-per-tick (deterministic).
2. `const auto inputId = predictor.applyInput(input);`
3. Local cube renders at `predictor.predictedState()` (instant).
4. Send `PlayerInputMsg{inputId, dx, dy, dz}` unreliable via
   `peers.send(0, ..., Unreliable)` (peerId 0 = host) — except if we
   ARE the host, in which case the input was already applied to our
   own authority state by step 5 below; no send needed.

**Host receives `PlayerInputMsg{inputId, dx, dy, dz}` from a client:**
1. Look up peerId via `peers.peerIdFor(c)`.
2. Apply via `playerStates[peerId] = simulate(playerStates[peerId], {dx, dy, dz}, dt)`.
3. Broadcast `AuthorityPositionMsg{peerId, x, y, z, inputId}` via
   `peers.broadcastToAll(..., Unreliable)`.

**Host's own input loop:**
- Same `PlayerInput` computed each tick.
- Apply via the same `simulate` to host's authoritative state
  (`playerStates[0]`).
- Broadcast `AuthorityPositionMsg{0, x, y, z, /*lastInputId=*/0}`
  to all clients.
- Host doesn't need a PredictionEngine for itself since its
  "predicted" state IS the authoritative state.

**Client receives AuthorityPositionMsg:**
- If `msg.peerId == myPeerId`: `predictor.reconcile(PlayerState{x,y,z}, msg.lastInputId)`.
- Else: `remoteHistories[msg.peerId].push(Vec3{x,y,z})` — existing
  TimeHistory flow for remote rendering (unchanged from M8.4).

**Tag-detection / scoring / round handling on host:** unchanged in
algorithm, just reads `playerStates[peerId]` instead of TimeHistory
samples (the data is more direct).

### Net-cubes refactor

Replace the manual onConnectionOpened/Closed lambdas, the HelloMsg
handler, the `connToPeerId`/`nextPeerId`/`myPeerId`/`isHost`/`hostConn`
state, the gameId validation, and the `peerCount` computation with
a `PeerManager` instance. The PositionMsg broadcast/receive logic stays
unchanged — still client-authoritative, still demonstrates the simpler
pattern.

Expected delta: ~80 lines removed, ~10 added. Behavior unchanged.

### Snapshot pattern (docs only)

New section in `docs/engine/networking.md` showing the recipe:

> When a new peer joins, the host needs to send them whatever world
> state they need to be in sync. Pattern:
>
> ```cpp
> peers.setOnPeerJoined([&](uint32_t newPeer) {
>     if (!peers.isHost()) return;
>     if (newPeer == 0 || newPeer == peers.myPeerId()) return;  // not "another peer"
>     // Send a snapshot of current round state:
>     peers.send(newPeer, RoundStartMsg{currentIt, remainingTimeSec},
>                SendReliability::Reliable);
>     for (const auto& [pid, score] : scores) {
>         peers.send(newPeer, ScoreUpdateMsg{pid, score},
>                    SendReliability::Reliable);
>     }
> });
> ```

Net-tag's existing late-joiner snapshot logic (from M8.3) is the
reference implementation; the refactor moves it into the onPeerJoined
callback cleanly.

## Testing

### `tests/test_peer_manager.cpp`

Paired MockTransports + MessageRegistries + PeerManagers, both
constructed with `kGameId = 0xABCD1234u`:

1. **Host + client connect** — both fire `onPeerJoined(peerId)` after polling. Verify host sees client's peerId; client sees `0` (the host).
2. **Wrong gameId** — client constructed with different gameId. After connect, client closes its side; host eventually sees onPeerLeft. Game would see this and exit.
3. **send/broadcast routing** — host sends a custom message to client via `peers.send(peerId, msg, Reliable)`; client receives it. Host broadcasts; all clients receive.
4. **Disconnect** — peer drops; `onPeerLeft(peerId)` fires.
5. **peerIds/connectionFor/peerIdFor** — basic accessor sanity.

### `tests/test_prediction_engine.cpp`

Uses trivial `TInput = int`, `TState = int` where `simulate(s, i, dt) = s + i` (dt unused).

1. **applyInput accumulates** — three calls with inputs 1, 2, 3 → predictedState == 6 (starting from 0); historySize == 3.
2. **Reconcile match** — apply 1, 2, 3 (state = 6); call `reconcile(authState=3, lastInputId=2)`. History should drop to just input 3; predictedState stays 6.
3. **Reconcile mismatch** — apply 1, 2, 3 (state = 6); call `reconcile(authState=100, lastInputId=2)`. History drops to just input 3; predictedState = 100 + 3 = 103.
4. **Reset** — wipe + check.

## Files

| Path | Action |
|------|--------|
| `engine/net/PeerMessages.h` | new |
| `engine/net/PeerManager.h` | new |
| `engine/net/PeerManager.cpp` | new |
| `engine/net/PredictionEngine.h` | new (header-only template) |
| `tests/test_peer_manager.cpp` | new |
| `tests/test_prediction_engine.cpp` | new |
| `games/05-net-cubes/Messages.h` | modify — delete HelloMsg |
| `games/05-net-cubes/main.cpp` | modify — adopt PeerManager (~80 lines dropped) |
| `games/06-net-tag/Messages.h` | modify — delete HelloMsg, add PlayerInputMsg + AuthorityPositionMsg, renumber TagSwap+ |
| `games/06-net-tag/main.cpp` | modify — PeerManager + PredictionEngine + server-authoritative position |
| `engine/CMakeLists.txt` | modify — add `net/PeerManager.cpp` |
| `tests/CMakeLists.txt` | modify — register two new tests |
| `docs/engine/networking.md` | modify — PeerManager + PredictionEngine + Snapshot pattern sections |

## Risks

- **Wire-format break for net-tag** — message tags renumber + new
  PlayerInput/AuthorityPosition replace the old PositionMsg. Old
  clients/hosts won't interop. Acceptable; we control both ends and
  there's no installed base.
- **`operator==` on `TState`** — exact float equality. Works for
  deterministic single-platform sims. If float drift ever matters
  (different optimisation levels client vs host? different CPU?), add
  a comparator-parameter constructor.
- **PredictionEngine in trivial games doesn't validate divergence** —
  net-tag's `simulate` is pure addition; predicted always matches
  authoritative; reconciliation never actually fires. We're proving the
  abstraction's interface, not its divergence-recovery. M8.6 hero
  shooter with capture-point collision will exercise mismatches for
  real.
- **PeerManager opinionates `onPeerJoined(0)` for self** — host fires
  `onPeerJoined(0)` synchronously inside `start()` so game's per-peer
  init code is uniform. Game code MUST set `onPeerJoined` callback
  before calling `start()`. Documented.
- **PeerManager destructor unhooks via setOn... handlers** —
  destruction order matters: registry/transport must outlive the
  PeerManager. Same constraint as `MessageRegistry`. Documented.

## Out of scope (M8.6+)

- `iron::SnapshotBroadcaster<T>` — pattern documented; no helper
- Lag-compensation game-side use of TimeHistory (M8.6 hero shooter)
- Hero shooter game itself
- Reconnect / host migration
- Tolerance-based prediction comparator
- Cross-platform float-determinism
- Per-shader once-per-frame uniform pass + `FrameContext` aggregate
  (engine-cleanup follow-ups noted across multiple milestone reviews;
   still pending)
