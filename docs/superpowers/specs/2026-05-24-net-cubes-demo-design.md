# Shared-cubes Networked Demo (M8.2) — Design

**Date:** 2026-05-24
**Track:** Networking — M8.2
**Status:** Approved — proceeding to implementation plan

## Motivation

M8.1 shipped `iron::NetTransport` + `iron::GnsTransport` and unit-tested the
wrapper against `iron::MockTransport`. What we have not yet done: drive the
wrapper from a real game loop with actual frame-paced send/receive
traffic. Ping-pong sent exactly two messages and exited. We don't know if
the wrapper's ergonomics, throughput, or callback model survive an
actual 60 FPS render loop until we try it.

This milestone builds a tiny multiplayer demo to find out:
`games/05-net-cubes` — a 3D scene where each peer is a 1m colored cube,
positions sync at ~30 Hz over the unreliable channel, and the user can
launch multiple exes to see real multiplayer rendering happen. The first
exe becomes host; later exes auto-connect.

The demo will also surface what's missing on the wrapper. We expect to
discover ergonomic gaps (e.g. "I need to broadcast to all connections"
helpers) and feed them back into M8.3 (typed-message system) and beyond.

## Goals

- Multiple `net-cubes.exe` instances on one machine see each other's cube
  positions update in real time.
- First instance becomes host automatically; later instances become
  clients automatically (try listen → fall back to connect).
- Star topology — clients only talk to host; host rebroadcasts.
- All wiring through `iron::GnsTransport` + the wrapper interface; no
  `<steam/...>` includes in the game.
- Free-fly camera with WASD + QE + mouse; HUD shows `Host` vs
  `Client (peer N)` and the live peer count.

## Non-goals

- **No typed-message dispatcher.** That is M8.3, which will refactor the
  cubes demo to use it. For now, dispatch is an inline switch on a 1-byte
  tag prefix.
- **No snapshot interpolation / client-side prediction / lag
  compensation.** Remote cubes snap to the latest received position;
  visible stutter is acceptable for proving the pipeline works.
- **No reconnect / host migration.** Host quits → clients exit.
- **No CI self-test mode.** The single-`GnsTransport`-per-process limit
  (discovered in M8.1 — process-global `GameNetworkingSockets_Init/Kill`)
  means a self-test would need the single-transport-loopback trick from
  ping-pong, adding ~50 lines for little extra coverage. The wrapper is
  already unit-tested by `test_mock_net_transport`.
- **No cross-machine play in scope.** Hard-coded `127.0.0.1`. LAN play
  works trivially if the user edits the port and changes the IP in source,
  but that's a stretch goal.
- **No Strandbound integration.** Separate demo only.

## Architecture

### File layout

```
games/05-net-cubes/
├── CMakeLists.txt
├── main.cpp           # window + renderer + scene + main loop + host/client logic
└── Protocol.h         # MsgTag enum + HelloMsg/PositionMsg structs + write/parse helpers
```

### Auto host/client detection

```cpp
iron::GnsTransport transport;
transport.start();

const iron::NetAddress addr{0x7F000001, 27015};
const bool becameHost = transport.listen(addr);
iron::ConnectionId hostConn = iron::kInvalidConnection;
if (!becameHost) {
    hostConn = transport.connect(addr);
    if (hostConn == iron::kInvalidConnection) { /* error + exit */ }
}
```

If `listen` succeeds, we are host. If `listen` fails (port already taken)
we connect to that same address — by convention the existing listener
must be a peer. (If the port is taken by an unrelated program, the demo
will misbehave; the user can change the port via the `NET_CUBES_PORT`
environment variable.)

### Protocol

Two message types, distinguished by a 1-byte tag prefix.

```cpp
// games/05-net-cubes/Protocol.h
namespace iron::netcubes {

enum class MsgTag : std::uint8_t {
    Hello    = 1,   // host -> new client; assigns the client's peerId
    Position = 2,   // bidirectional; sent ~30 Hz unreliable
};

struct HelloMsg     { std::uint32_t peerId; };
struct PositionMsg  { std::uint32_t peerId; float x, y, z; };

// Pack `msg` into `out` as `[tag][msg bytes]`. `out` is replaced.
void writeHello(std::vector<std::byte>& out, HelloMsg msg);
void writePosition(std::vector<std::byte>& out, PositionMsg msg);

// Parse the message in `bytes`. Returns the tag (Hello or Position) and
// fills the appropriate output struct. Returns std::nullopt if `bytes`
// doesn't match either expected length / tag.
struct ParsedMsg {
    MsgTag tag;
    HelloMsg hello;            // valid if tag == Hello
    PositionMsg position;      // valid if tag == Position
};
std::optional<ParsedMsg> parse(std::span<const std::byte> bytes);

}  // namespace iron::netcubes
```

Wire format (little-endian, packed; the project is single-platform MSVC
so endianness is fixed):
- HelloMsg: `[tag=1 u8][peerId u32]` = 5 bytes
- PositionMsg: `[tag=2 u8][peerId u32][x f32][y f32][z f32]` = 17 bytes

`memcpy` is used for serialisation — `Protocol.h` does NOT depend on
any engine type beyond `std::span<const std::byte>` and `iron::Vec3`
(used only as a convenience constructor; the wire format stores plain
floats so a future endianness fix or cross-platform port doesn't have to
touch `Vec3`).

Reliability:
- `Hello` — reliable (the client cannot proceed without its peerId).
- `Position` — unreliable (newer overrides older; drops are harmless).

### Identity

- Host's `peerId` is `0`.
- Host increments a counter starting at `1` for each accepted client and
  sends them a `HelloMsg` with that ID.
- Every `PositionMsg` carries the sender's `peerId`, so receivers know
  which cube to update.

### Host behaviour

State:
- `std::unordered_map<iron::ConnectionId, std::uint32_t> connToPeerId_`
- `std::unordered_map<std::uint32_t, Vec3> cubes_` — every peer's cube, including the host's (peer 0)
- `std::uint32_t nextPeerId_ = 1`
- `Vec3 myPos_` — host's own cube position, kept in sync with the camera

Callbacks:
- `onConnectionOpened(c)` — assign `nextPeerId_++`, store in
  `connToPeerId_`, write a `HelloMsg{peerId}` to a reused buffer, send it
  on connection `c` *reliable*.
- `onMessage(c, bytes)` — `parse(bytes)`. If `Position`:
  1. Update `cubes_[msg.peerId] = {msg.x, msg.y, msg.z}`.
  2. Rebroadcast the same byte buffer to every *other* connected client
     (skip `c`) using unreliable send.
  Hello messages on the host side are an error — ignore with a warning.
- `onConnectionClosed(c, reason)` — look up `connToPeerId_[c]`, remove
  `cubes_[peerId]`, remove the map entry.

Per-frame tick (after camera update, before render):
- Update `myPos_` from camera position.
- Update `cubes_[0] = myPos_`.
- If at least 33 ms have passed since last broadcast: write a
  `PositionMsg{0, myPos_.x, myPos_.y, myPos_.z}` and send unreliable to
  every connected client.

### Client behaviour

State:
- `iron::ConnectionId hostConn_`
- `std::uint32_t myPeerId_ = 0` (0 means "not yet assigned")
- `std::unordered_map<std::uint32_t, Vec3> cubes_`
- `Vec3 myPos_`

Callbacks:
- `onConnectionOpened(c)` — assert `c == hostConn_`; no other action
  (waiting for `HelloMsg`).
- `onMessage(c, bytes)` — `parse(bytes)`. If `Hello` and
  `myPeerId_ == 0`, set `myPeerId_ = msg.peerId`. If `Position`, update
  `cubes_[msg.peerId]`. Otherwise warn.
- `onConnectionClosed(c, reason)` — print error, set a `done` flag,
  the main loop exits cleanly.

Per-frame tick:
- Update `myPos_` from camera.
- If `myPeerId_ != 0` and at least 33 ms since last send: write a
  `PositionMsg{myPeerId_, myPos_.x, myPos_.y, myPos_.z}` and send
  unreliable on `hostConn_`.

### Scene & rendering

| Element | Notes |
|---------|-------|
| Ground plane | 40×40 quad at y=0, normal +Y; uses `assets/cc0/ground/` PBR triple |
| Skybox | Procedural sunset cubemap (existing `generateSunsetFace` from showcase) |
| Cubes | 1m `appendBox` mesh; one DrawCall per peer; emissive colour from `hash(peerId)` |
| Sun | Directional, slight angle, ambient 0.2 |
| Camera | `iron::FreeFlyCamera` starting at `{8, 4, 12}` looking at origin |
| HUD | top-left text: `Host` or `Client (peer N)`; one line below: `Peers: N`; rendered via existing `iron::Hud` |

Cube colour derivation:
```cpp
Vec3 colorForPeer(std::uint32_t peerId) {
    // Spread peer IDs across the hue circle using a simple golden-ratio
    // hash so adjacent IDs produce distinct hues.
    const float hue = std::fmod(peerId * 0.61803398875f, 1.0f);
    // hsv2rgb with saturation=0.8, value=0.9
    ...
}
```

Cube model matrix:
```cpp
iron::translation(cubes_[peerId])
```

Shader: reuse the existing strandbound lit shader (TBN, Blinn-Phong, point
lights = empty, fog off, no reflection). Diffuse is the cube colour times
1.0 (`Material::texture = renderer.whiteTexture()`, `Material::emissive = color * 0.3`).

### CMake

`games/05-net-cubes/CMakeLists.txt`:
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

Top-level `CMakeLists.txt` gets `add_subdirectory(games/05-net-cubes)`
after the existing `games/04-net-pingpong` line.

## Testing

No new unit test. The networking wrapper is already covered by
`test_mock_net_transport` (M8.1). CI will build `net-cubes.exe` as part
of the regular build step; verification of multiplayer behaviour is
manual (launch 2-4 copies of the exe; watch the cubes move).

## Files

| Path | Action |
|------|--------|
| `games/05-net-cubes/CMakeLists.txt` | new |
| `games/05-net-cubes/main.cpp` | new (~250 lines: scene setup, main loop, host/client logic) |
| `games/05-net-cubes/Protocol.h` | new (~80 lines header-only: tag + structs + write/parse) |
| `CMakeLists.txt` (top-level) | modify — `add_subdirectory(games/05-net-cubes)` |
| `docs/engine/networking.md` | modify — add a "Play with it" section pointing at cubes |

## Risks

- **Port collision** — if `127.0.0.1:27015` is held by an unrelated
  process, our auto-detection will connect to it and the protocol will
  misbehave. Mitigation: read `NET_CUBES_PORT` env var to override the
  port at startup. Minimum-viable: document in stderr if connect
  succeeds but no `HelloMsg` arrives within 2 s.
- **Snap-step on remote cubes** — 30 Hz unreliable updates with no
  interpolation. Acceptable per non-goals; pretty fix is M8.3+.
- **Self-test gap** — wrapper has unit tests but the demo itself has no
  automated regression test. Build success in CI is the only check.
- **Endianness assumption** — wire format is host-endian. Project is
  Windows MSVC only so this is moot today; future cross-platform port
  will need `htonl`-style fixes.
- **Host disappears** — clients exit on `onConnectionClosed(host)`.
  Acceptable for a tech demo.

## Out of scope (M8.3+)

- Typed-message dispatcher (`iron::MessageRegistry<...>` with handlers
  keyed by enum / type), refactor of the cubes demo to use it
- Snapshot interpolation, client-side prediction, lag compensation
- Reconnect / host migration / mid-session join
- Strandbound integration (multiplayer game)
- Steam Datagram Relay, Steam matchmaking
- Cross-machine LAN testing as a first-class feature
