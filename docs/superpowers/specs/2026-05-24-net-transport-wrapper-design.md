# Engine Networking Wrapper (M8.1) — Design

**Date:** 2026-05-24
**Track:** Networking — M8.1
**Status:** Approved — proceeding to implementation plan

## Motivation

M8.0 proved GameNetworkingSockets builds and works (PR #18, `5e25fbf`).
What it did NOT do: provide any engine-side abstraction. `games/04-net-pingpong/main.cpp`
talks to the raw GNS C API, uses a file-scope `g_state` pointer for callback
dispatch, and would need to be touched anywhere GNS API changes. Game code
today has zero protection from the underlying transport.

This milestone closes that gap by mirroring the engine's existing
renderer pattern: an abstract `iron::NetTransport` base in `engine/net/`
with a concrete `iron::GnsTransport` (and a `iron::MockTransport` for
tests) under `engine/net/backends/`. Game code holds a
`NetTransport*` and never includes GNS headers.

## Goals

- Game code can open a listener, connect to a peer, send + receive reliable
  bytes, and observe lifecycle events without touching GNS headers.
- Unit tests can exercise the same `NetTransport` contract against a
  `MockTransport` — no real sockets, deterministic, instant.
- Rewrite `games/04-net-pingpong/main.cpp` on top of the wrapper as the
  proof-of-life integration test (~80 lines, down from ~240). CI keeps
  running it.
- The two HIGH bugs flagged in PR #18 review (handle leak on Connecting
  failure; dangling handle after close-in-callback) die at the source
  when the wrapper centralises connection lifecycle.

## Non-goals

- **No protocol / message schema.** Game still serialises raw bytes. Typed
  messages, versioning, length-prefixing are M8.2+ concerns.
- **No replication / "network variables".** That's the layer above
  transport; needs real game integration to ground decisions.
- **No tick / sim model.** No fixed timestep, snapshots, prediction,
  rollback. M8.2+.
- **No Steam Datagram Relay, no Steam auth, no NAT punching.** GNS
  standalone only, localhost only.
- **No game integration.** Strandbound and showcase stay single-player.
- **No threading model change.** `NetTransport::poll()` is called from the
  game's main thread, same as today.
- **No cross-machine testing.** Still localhost-only via ping-pong exe.

## Architecture

### File layout — mirrors `engine/render/`

```
engine/net/
├── NetTransport.h         # abstract base + types (NetAddress, ConnectionId, SendReliability)
├── NetTransport.cpp       # trivial — any out-of-line stubs needed
├── backends/
│   ├── gns/
│   │   ├── GnsTransport.h
│   │   └── GnsTransport.cpp     # owns ISteamNetworkingSockets*; wraps the C-API
│   └── mock/
│       ├── MockTransport.h
│       └── MockTransport.cpp    # paired in-memory impl for tests
```

`ironcore` links GNS (transitively). Every game built against ironcore picks
up GNS whether they use it or not — same pattern as glfw/glad. Accepting
the ~5-10 MB binary growth keeps the engine library cohesive; splitting
out `ironcore-net` would be premature factoring.

### Interface

```cpp
namespace iron {

using ConnectionId = std::uint32_t;
constexpr ConnectionId kInvalidConnection = 0;

// Network endpoint. IPv4 only today; IPv6 / hostnames are future.
struct NetAddress {
    std::uint32_t ipv4 = 0x7F000001;  // 127.0.0.1 default
    std::uint16_t port = 0;
};

enum class SendReliability {
    Reliable,    // retransmit + in-order
    Unreliable,  // best-effort, no ordering
};

// Transport-agnostic networking interface. One concrete implementation
// today (GnsTransport over GameNetworkingSockets); MockTransport exists
// for tests. Game code holds a NetTransport* and never includes GNS
// headers.
//
// Driving model:
//   1. Construct a concrete transport.
//   2. Install observer callbacks via the setOn... methods.
//   3. start() the transport.
//   4. listen(addr) or connect(addr) (or both — server may also be a client).
//   5. Each game-loop tick: call poll(). The transport fires the observer
//      callbacks for any state changes / incoming messages.
//   6. send() to push bytes; close() to drop a connection; stop() to shut down.
class NetTransport {
public:
    virtual ~NetTransport() = default;

    // --- lifecycle ---
    // Initialise the underlying transport. Returns false on failure
    // (e.g. GameNetworkingSockets_Init refused). Idempotent: start()
    // after a successful start() is a no-op and returns true.
    virtual bool start() = 0;

    // Shut down. Closes all open connections. Idempotent.
    virtual void stop() = 0;

    // --- endpoints ---
    // Bind a listener at addr. Returns false on failure (port taken,
    // bad address). New clients arrive via onConnectionOpened.
    virtual bool listen(NetAddress addr) = 0;

    // Initiate a connection to a remote endpoint. Returns a ConnectionId
    // immediately. The connection is NOT open yet — onConnectionOpened
    // fires when the handshake completes, or onConnectionClosed fires if
    // it fails. Returns kInvalidConnection on synchronous failure
    // (transport not started, etc.).
    virtual ConnectionId connect(NetAddress addr) = 0;

    // --- I/O ---
    // Send a byte buffer on `conn`. `reliability` chooses the channel.
    // Returns false if `conn` is invalid/closed (no callback fired in
    // that case). The buffer is copied internally — caller can free
    // immediately after return.
    virtual bool send(ConnectionId conn,
                      std::span<const std::byte> bytes,
                      SendReliability reliability) = 0;

    // Close one connection. Fires onConnectionClosed on the peer.
    // Local-side onConnectionClosed does NOT fire (the local code asked
    // for the close; it already knows).
    virtual void close(ConnectionId conn) = 0;

    // Drive the transport: dispatch status callbacks, drain inbound
    // message queues. Game calls this once per main-loop tick.
    virtual void poll() = 0;

    // --- observers ---
    // Set once after construction, before start(). Callbacks fire on the
    // thread that calls poll().
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

### `GnsTransport` — the concrete implementation

Owns:
- One `ISteamNetworkingSockets*` (acquired from `SteamNetworkingSockets()` after `GameNetworkingSockets_Init`)
- One `HSteamNetPollGroup` for all accepted connections (server side) — so message draining is one call per poll
- A `std::unordered_map<HSteamNetConnection, ConnectionId>` translating GNS handles to opaque engine IDs
- A monotonically increasing counter for fresh `ConnectionId` values
- The three observer callbacks

Dispatching status changes:
- `start()` installs the C-style status-changed callback via
  `k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged`. The callback
  function is a static method that uses
  `info->m_info.m_nUserData` (set on the connection at create-time) to recover
  the `GnsTransport*` instance. **No file-scope `g_state` pointer.**
- The instance method then translates the GNS handle to the engine
  `ConnectionId` and fires the appropriate observer callback.

`send()`:
- Looks up the GNS handle from `ConnectionId`. Returns false if unknown.
- Calls `SendMessageToConnection` with the appropriate send flag.
- Copies the buffer (we own the contract that the caller may free after
  the call).

`close()`:
- Looks up the GNS handle, calls `CloseConnection`, removes the mapping.
- Does NOT fire the local `onConnectionClosed` (per the interface contract).

`poll()`:
- `sockets->RunCallbacks()` — drives status changes.
- Drain inbound: `sockets->ReceiveMessagesOnPollGroup(...)`, fire
  `onMessage` for each, `Release()` the message.

`stop()` and destructor:
- Close every tracked connection, destroy the poll group, close the listen
  socket(s), `GameNetworkingSockets_Kill()`.

The two HIGH bugs from PR #18 review (handle leak on `Connecting`-branch
`SetConnectionPollGroup` failure; dangling-handle double-close after
close-in-callback) live in one place in `GnsTransport` instead of being
scattered across every game's main loop. Fixed once, fixed forever.

### `MockTransport` — for tests

A pair of `MockTransport` instances connect to each other in-memory:
- Each instance has a `std::queue<PendingEvent>` of inbound events
  (connection-opened, message, connection-closed) and a pointer to its
  peer's queue.
- `connect(addr)` registers the local + peer event pair; the peer's
  `onConnectionOpened` fires on its next `poll()`. (Addresses are ignored
  in the mock — it's a single in-process loopback.)
- `send()` enqueues an `onMessage` event into the peer's queue.
- `close()` enqueues an `onConnectionClosed` into the peer's queue.
- `poll()` drains and dispatches.

Reliable preserves in-order delivery (FIFO queue). Unreliable is treated
the same in the mock — the contract test verifies ordering for Reliable;
the mock makes no claim about packet drops. (Future enhancement: a
fault-injecting mock that drops/reorders Unreliable. Out of scope today.)

### Ping-pong rewrite

`games/04-net-pingpong/main.cpp` shrinks to ~80 lines. Pseudocode:

```cpp
int main() {
    iron::GnsTransport transport;
    bool done = false;
    bool failed = false;

    transport.setOnConnectionOpened([&](iron::ConnectionId c) {
        // Client-side: when our connect succeeds, send PING.
        // (Both sides see opened events; the listener side ignores it
        //  because the message handler does all the work.)
        if (c == clientConn) {
            transport.send(c, asBytes("PING"), iron::SendReliability::Reliable);
        }
    });
    transport.setOnMessage([&](iron::ConnectionId c, std::span<const std::byte> bytes) {
        std::string_view msg{reinterpret_cast<const char*>(bytes.data()),
                              bytes.size()};
        if (msg == "PING") {
            transport.send(c, asBytes("PONG"), iron::SendReliability::Reliable);
        } else if (msg == "PONG") {
            done = true;
        } else {
            failed = true;
        }
    });
    transport.setOnConnectionClosed(/* fail on unexpected close */);

    transport.start();
    transport.listen({0x7F000001, 27015});
    const iron::ConnectionId clientConn = transport.connect({0x7F000001, 27015});

    const auto deadline = now() + 5s;
    while (!done && !failed && now() < deadline) {
        transport.poll();
        std::this_thread::sleep_for(10ms);
    }

    transport.stop();
    if (done) { std::printf("OK\n"); return 0; }
    return 1;
}
```

Same exit code semantics (0 = OK, 1 = fail). Same 5-second deadline. CI
keeps running it as the integration test.

The ping-pong exe no longer needs to link GNS directly — it links
`ironcore` and gets GNS transitively.

## Testing

`tests/test_mock_net_transport.cpp` exercises the `NetTransport` contract
against `MockTransport`:

1. **Pair of mocks connects, both `onConnectionOpened` fire.**
2. **Send delivers**: A sends bytes to B, A polls (no-op), B polls,
   `onMessage` fires on B with the same bytes.
3. **In-order delivery** for Reliable: A sends 3 distinct messages
   back-to-back, B polls once, `onMessage` fires 3 times in send order.
4. **Close fires onConnectionClosed on peer**: A closes the connection,
   B polls, B's `onConnectionClosed` fires.
5. **Send on closed connection returns false** (no callback, no crash).
6. **Send on invalid id returns false** (e.g. `kInvalidConnection`).

No CTest entry for the real-socket ping-pong — that exe IS the
integration test, like today.

## Files

| Path | Action |
|------|--------|
| `engine/net/NetTransport.h` | new — abstract interface + `NetAddress`, `ConnectionId`, `SendReliability` |
| `engine/net/NetTransport.cpp` | new — any non-virtual definitions (may end up trivial; create anyway to keep CMake symmetric) |
| `engine/net/backends/gns/GnsTransport.h` + `.cpp` | new — GNS-backed impl |
| `engine/net/backends/mock/MockTransport.h` + `.cpp` | new — in-memory paired impl |
| `engine/CMakeLists.txt` | modify — add 4 new `.cpp` files; `target_link_libraries(ironcore PUBLIC ... GameNetworkingSockets::GameNetworkingSockets)` |
| `games/04-net-pingpong/CMakeLists.txt` | modify — drop direct GNS link (now transitive via ironcore); link `ironcore` |
| `games/04-net-pingpong/main.cpp` | rewrite on the wrapper (~80 lines) |
| `tests/test_mock_net_transport.cpp` | new |
| `tests/CMakeLists.txt` | modify — `iron_add_test(test_mock_net_transport ...)` |
| `docs/engine/networking.md` | new — short intro + the architecture sketch from §"File layout" + the driving-model bullets from the interface comment |

## Risks

- **Interface designed by ping-pong's needs.** A real networked game
  (M8.2+) may need methods we didn't add (per-connection ping/RTT,
  bandwidth stats, configurable timeouts). Mitigation: the interface is
  in our own codebase, evolve as needed.
- **`std::function` allocation overhead.** Capturing lambdas heap-allocate
  for the closure. Fine at game-loop frequency for status callbacks; for
  message dispatch we'll see whether it shows up in profiling before
  optimising.
- **Mock-vs-real semantic gap.** MockTransport tests the wrapper contract
  but not GNS-specific behaviour (genuine packet loss, encryption
  handshake, retransmits). Acceptable — those are GNS's responsibility,
  and the ping-pong exe is our integration check.
- **PUBLIC link of GNS into ironcore.** Every game built on ironcore picks
  up GNS as a transitive dep. If link time becomes a concern we split out
  `ironcore-net` later; not today.
- **Connection-id mapping race.** The status-changed callback fires from
  the same thread that calls `poll()` (since GNS only dispatches from
  `RunCallbacks`), so no locking is needed today. If we ever move
  networking to its own thread we'll revisit.

## Out of scope (M8.2+)

- Protocol / typed messages / schema / versioning
- Snapshot interpolation, lockstep, rollback
- Entity replication / "network variables"
- Tick scheduling, deterministic sim
- Steam Datagram Relay, Steam auth, matchmaking
- IPv6, hostname resolution
- Cross-machine testing
- Game integration (multiplayer Strandbound is several milestones out)
- Per-connection bandwidth/RTT stats accessors
