# Engine networking

The engine exposes a transport-agnostic networking interface,
`iron::NetTransport`, with two concrete implementations:

- `iron::GnsTransport` — production. Wraps Valve's
  [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets):
  UDP with reliable + unreliable channels, encryption, congestion control,
  fragmentation. Works standalone (no Steam needed). One per process —
  `GameNetworkingSockets_Init/Kill` are process-global lifecycle.
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
needs (`net/backends/gns/GnsTransport.h` for production,
`net/backends/mock/MockTransport.h` for tests). It never includes any
`<steam/...>` headers — those live entirely inside `GnsTransport.cpp`.

`ironcore` PUBLIC-links GameNetworkingSockets, so any game built against
`ironcore` picks it up transitively. No per-game CMake change needed.

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

A single `GnsTransport` can both listen AND connect at the same time — a
client connection from `connect()` on the same instance loops back to its
own listen socket. That is how the `04-net-pingpong` smoke test works in
a single process.

## What this layer does NOT do (yet)

- No protocol / typed messages — you send raw bytes.
- No replication / "network variables".
- No tick scheduling, snapshot interpolation, prediction, rollback.
- No Steam Datagram Relay, no NAT traversal.
- No IPv6 or hostname resolution — IPv4 only.

These land in later milestones once game integration grounds the design.

## Reliability semantics

`SendReliability::Reliable` retransmits lost packets and preserves
in-order delivery within the connection — like TCP, but as discrete
messages rather than a byte stream.

`SendReliability::Unreliable` is fire-and-forget: GNS makes a best
effort to deliver but won't retransmit, and messages may arrive out of
order or be dropped silently. Use it for state that supersedes itself
every frame (position snapshots, animation pose).

`MockTransport` treats both modes as in-order FIFO — it makes no claim
about emulating packet loss for unreliable. A fault-injecting mock for
that case is a future enhancement.

## The smoke test

`games/04-net-pingpong/main.cpp` is a single-process exe that uses one
`iron::GnsTransport` instance to open a listener and a client-back-to-self
connection on `127.0.0.1:27015`, exchange PING + PONG reliably, and exit
with code 0 (or 1 + a stderr diagnostic on any failure). CI runs it as
part of every build — it's the end-to-end check that the wrapper works
against real sockets.

The unit test for the contract — `tests/test_mock_net_transport.cpp` —
runs the same kind of paired-connect / send / receive / close scenarios
against `MockTransport` (no real sockets) on every CTest invocation.

## Play with it: net-cubes

`games/05-net-cubes` is a runnable multiplayer demo built on the wrapper.
Launch two or more copies of `net-cubes.exe` on the same machine:

- The first instance binds the listen socket and becomes the host (peer 0).
- Every later instance auto-detects that the port is taken and connects
  as a client. The host assigns each new client an incrementing `peerId`
  via a one-shot reliable `HelloMsg`.
- Each peer is a 1m colored cube; positions sync at ~30 Hz over the
  unreliable channel. Move with WASD + QE + mouse; ESC quits.
- Star topology: clients only talk to the host. The host rebroadcasts
  each client's position to every other client.

The protocol is two messages (Hello and Position) declared in
`games/05-net-cubes/Messages.h` as POD structs with a `static constexpr
std::uint8_t kTag`. Dispatched via `iron::MessageRegistry` — see the
"Typed messages" section below.

## Typed messages: `iron::MessageRegistry`

Game code does NOT manually serialise bytes and switch on a tag byte.
That's what the registry is for. Define messages as POD structs with a
1-byte tag:

```cpp
struct PositionMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t peerId;
    float x, y, z;
};
```

Then wrap a transport once, register handlers per type, and send by type:

```cpp
iron::GnsTransport transport;
iron::MessageRegistry registry(&transport);

registry.registerHandler<PositionMsg>([&](iron::ConnectionId c, const PositionMsg& m) {
    cubes[m.peerId] = {m.x, m.y, m.z};
});

// Later:
registry.send<PositionMsg>(connId, PositionMsg{myId, 1.0f, 0.5f, -2.0f},
                            iron::SendReliability::Unreliable);
```

Wire format is `[u8 tag][raw memcpy of struct]`. Tag 0 is reserved.
Messages must be trivially copyable (POD only — no `std::string`, no
`std::vector` inside). Compile-time enforced via `static_assert`. Max
payload is 1200 bytes (one message, one packet, no fragmentation).

Tag namespaces are per-registry — different games can reuse the same tag
values without conflict.

## Cross-machine LAN play

Both networked demos (`net-cubes` and `net-tag`) parse the same CLI flags
via `iron::parseNetArgs(argc, argv)`:

```
net-cubes.exe                                # listen on 30005 (host)
net-cubes.exe --connect 192.168.1.5          # client connecting to that IP
net-tag.exe --connect 127.0.0.1 --port 40000
```

No args = listen. `--connect <ip>` = client mode targeting that IP.
`--port <n>` = override the default port (30005) on either side. Bad
inputs (malformed IPs, non-numeric ports) fall back to defaults with a
warning rather than aborting.

## Play with it: net-tag

`games/06-net-tag` — N-player tag with a 60-second round and a live
leaderboard. Built entirely on `iron::MessageRegistry`. Cross-machine
LAN play via `--connect <ip>` / `--port <n>`.

Host is authoritative: detects the tag (distance < 1.5m + 0.5s cooldown
on both swap participants), runs the round timer, owns the scoreboard,
broadcasts state. Clients send position; receive everything else.

Cube colored by `peerId` via hue rotation; the "it" cube has a strong
red emissive glow so it's instantly readable across the scene. HUD shows
role + peers + game state + sorted leaderboard.

If the player who's "it" disconnects mid-round, the host hands "it" off
to a remaining player so the round can keep going.

## Snapshot interpolation: `iron::TimeHistory<T>`

Remote players don't render at the latest received position — they render
at `now - displayDelay` (default 100 ms), interpolated through a buffer
of recent samples. This trades 100 ms of viewing latency for smoothness:
network jitter and the occasional dropped packet disappear from the
visuals.

Game code:

```cpp
std::unordered_map<std::uint32_t, iron::TimeHistory<iron::Vec3>> remoteHistories;
constexpr auto kDisplayDelay = std::chrono::milliseconds{100};

// On every incoming PositionMsg from a remote peer:
remoteHistories[peerId].push(iron::Vec3{msg.x, msg.y, msg.z});

// Per-frame render:
for (const auto& [peerId, history] : remoteHistories) {
    auto pos = history.sampleAtDelay(kDisplayDelay);
    if (pos) {
        renderCube(*pos);
    }
}
```

The local player NEVER goes through the buffer — your own movement
should be instant. Render the local cube directly at `player.position`.

`TimeHistory<T>` is also used **server-side** for lag-compensated hit
detection (future hero-shooter milestone): host maintains a history of
every player's authoritative position; on `FireMsg{origin, dir,
fireTimestamp}`, the server queries `history.sample(fireTimestamp)` for
each player and validates hits against where they actually were when
the client fired. The primitive is here today.

`T` must support free-function `interpolate(T, T, float)` found via ADL.
The engine provides one for `iron::Vec3` in `engine/math/Vec.h`.

## Game-id handshake

Every networked game advertises a 4-byte ASCII identifier in its
`HelloMsg`. The client rejects a Hello whose `gameId` doesn't match.
This prevents accidentally connecting two different iron-core network
exes to each other (the cubes/tag interop bug we hit in M8.3 playtest).

Game IDs in use today:

| Game | gameId (hex) | ASCII |
|------|--------------|-------|
| net-cubes | `0x6E457442` | `'nEtB'` |
| net-tag | `0x7441476F` | `'tAGo'` |

When adding a new networked game, pick a 4-character ASCII string that
isn't in this table.

## Fixed-timestep simulation: `iron::FixedTickScheduler`

Game code that wants a deterministic tick rate (independent of render
framerate) uses `iron::FixedTickScheduler`:

```cpp
iron::FixedTickScheduler scoreTicker{std::chrono::seconds{1}};

while (running) {
    const float dt = frameDt();
    scoreTicker.update(dt, [&]() {
        broadcastScores();  // called exactly once per second of game time
    });
    render();
}
```

The scheduler accumulates `dt` and invokes the callback once per full
tick interval. On a long frame, it catches up by firing several times
in a row. On a short frame, it might not fire at all. The interval is
honored on average regardless of framerate jitter. Internally uses
integer microsecond arithmetic so the accumulator does not drift over
long runs.

## Network debug HUD

Both networked demos render a small "network stats" widget in the
top-right corner with the live state, ping, packet loss, and in/out
bandwidth for the current connection. Powered by:

```cpp
iron::ConnectionStats GnsTransport::stats(iron::ConnectionId conn) const;
```

which wraps `ISteamNetworkingSockets::GetConnectionRealTimeStatus`. The
`iron::Hud::addNetworkStatsWidget(...)` / `updateNetworkStats(...)` pair
wraps this into 3 lines of game code:

```cpp
auto netStatsHud = hud.addNetworkStatsWidget({1268, 12});
// each frame:
hud.updateNetworkStats(netStatsHud, transport.stats(activeConn));
```

Useful when diagnosing "is this peer actually connected?", "why is the
sync jittery?", or "is bandwidth blowing up?".
