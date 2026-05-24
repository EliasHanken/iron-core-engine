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
