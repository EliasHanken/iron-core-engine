# Networking Foundations (M8.0) — Design

**Date:** 2026-05-24
**Track:** Networking (new track — first milestone)
**Status:** Approved — proceeding to implementation plan

## Motivation

The visuals track shipped (PR #17, 2026-05-24). The next major track is
networking + multiplayer, flagged early in the session as a longer-term goal.

Networking is the kind of subsystem that's painful to retrofit: tick model,
entity IDs, input handling, save/load — all of these get touched by
multiplayer design. Building audio or scene-loading first only to refactor
them later for network awareness is wasteful. Network-first now means
future engine features get built with replication in mind from day one.

The chosen transport is **Valve's GameNetworkingSockets (GNS)**: reliable +
unreliable channels, encryption, fragmentation, congestion control already
solved by people who shipped Dota 2 / CS:GO; standalone (no Steam
dependency); optional Steam Datagram Relay later if shipping on Steam.
BSD-3 licensed, cross-platform.

This milestone (M8.0) is **purely plumbing**: get GNS to build, link, and
prove a single message can travel through it on localhost. No engine
abstraction, no game integration. M8.1+ will add `iron::NetTransport`
and start integrating with `Scene`.

## Goals

- `vcpkg.json` manifest brings in `gamenetworkingsockets` (and its transitive
  deps: `libsodium`, `protobuf`, `abseil`) reproducibly.
- `cmake --build build --target net-pingpong` produces a runnable exe.
- Running the exe creates a listener and a client, exchanges one message
  reliably, prints `OK`, exits with code 0.
- CI builds and runs the smoke test on Windows / MSVC.
- A short doc tells future-self (and contributors) how to bootstrap vcpkg
  locally.

## Non-goals

- **No engine abstraction.** No `iron::NetTransport`, `iron::NetVariable`,
  `iron::Replicated<T>` — those are M8.1+. The point of this milestone is to
  feel the GNS API directly before designing over it.
- **No two-process test.** Single-process server + client on localhost is
  enough to prove init + connect + send + receive.
- **No protocol design.** The smoke test sends a single hardcoded byte
  buffer. No header, no version field, no schema.
- **No Steam integration.** GNS standalone only. Steam Datagram Relay
  bootstrapping (auth, account, relay) is a separate later milestone.
- **No threading model.** GNS is driven by polling
  `ISteamNetworkingSockets::RunCallbacks()` on the calling thread; the smoke
  test does this from `main`.
- **No game integration.** Strandbound and the showcase stay single-player.

## Architecture

### Dependency management (vcpkg manifest mode)

```
iron-core-engine/
├── vcpkg.json                 # NEW — declares gamenetworkingsockets dep
├── vcpkg-configuration.json   # NEW — pins the vcpkg registry baseline
└── CMakeLists.txt             # modify: find_package(GameNetworkingSockets CONFIG REQUIRED)
```

**`vcpkg.json`:**
```json
{
  "name": "iron-core-engine",
  "version-string": "0.0.0",
  "dependencies": [
    "gamenetworkingsockets"
  ]
}
```

By default `gamenetworkingsockets` resolves with `libsodium` as the crypto
backend, which is exactly what we want.

**`vcpkg-configuration.json`** pins a specific vcpkg baseline SHA so
everyone gets the same versions:
```json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "<pinned-sha>"
  }
}
```
The implementer picks a recent baseline at execution time (`vcpkg x-update-baseline --add-initial-baseline`).

**Top-level `CMakeLists.txt`** gets one new block, after the existing
`FetchContent_MakeAvailable(glfw glad)`:
```cmake
find_package(GameNetworkingSockets CONFIG REQUIRED)
```
We do NOT link GNS into `ironcore` in this milestone — only the new
`net-pingpong` exe links it. Keeps the engine library free of network deps
until M8.1.

### Local developer setup

The user must invoke CMake with the vcpkg toolchain file once:
```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
```
After that, subsequent `cmake --build build` calls work as today.

A new `docs/engine/build-setup.md` covers:
1. Clone vcpkg (`git clone https://github.com/microsoft/vcpkg && ./vcpkg/bootstrap-vcpkg.bat`).
2. Configure CMake with the toolchain file.
3. First configure takes ~5–10 minutes (vcpkg builds GNS + libsodium + protobuf + abseil from source). Subsequent configures use the vcpkg binary cache and are seconds.

### CI update

The existing GitHub Actions workflow (`Build & test (Windows / MSVC)`) gets
two new steps before the `cmake -S . -B build ...` step:

1. **Setup vcpkg** — either `microsoft/vcpkg` action, or a manual checkout +
   `bootstrap-vcpkg.bat` invocation. Pin the same baseline SHA as
   `vcpkg-configuration.json`.
2. **Cache the vcpkg binary cache** — `actions/cache` keyed on the
   `vcpkg.json` SHA so subsequent CI runs don't rebuild GNS from source.

The configure step gains:
```yaml
-DCMAKE_TOOLCHAIN_FILE=${{ github.workspace }}/vcpkg/scripts/buildsystems/vcpkg.cmake
```

The smoke test runs as part of CI by adding:
```yaml
- run: ./build/games/04-net-pingpong/Debug/net-pingpong.exe
```

Exit code 0 on success; non-zero fails the CI step.

### The ping-pong exe (`games/04-net-pingpong/main.cpp`)

Single process. About ~150 lines including comments. Two GNS endpoints on
the same `127.0.0.1:port`:

```
main()
 ├─ Init GNS (GameNetworkingSockets_Init)
 ├─ Get ISteamNetworkingSockets* interface
 ├─ CreateListenSocketIP on 127.0.0.1:auto-port
 ├─ CreatePollGroup (so we can dequeue messages cleanly)
 ├─ ConnectByIPAddress to the same port from the client
 ├─ Pump RunCallbacks() until both sides report kConnectionState_Connected
 │    └─ status-change callback: when the server-side accepts the client
 │       connection, also assign it to the poll group
 ├─ client.SendMessageToConnection("PING", reliable)
 ├─ Pump RunCallbacks() + ReceiveMessagesOnPollGroup() until server gets PING
 ├─ server.SendMessageToConnection("PONG", reliable)
 ├─ Pump until client receives PONG
 ├─ Verify both messages matched
 ├─ Print "OK" to stdout
 ├─ Cleanup (CloseConnection, CloseListenSocket, DestroyPollGroup, GameNetworkingSockets_Kill)
 └─ return 0  (or 1 on any failure or 2s timeout)
```

GNS uses C-style callbacks for connection status changes. The standard
pattern (per Valve's official example) is to store a `g_pingpong = this`
file-scope pointer so the callback can route the event back to a method.
This is uglier than a member function but matches the API; don't fight it.

A 2-second wall-clock timeout aborts with exit code 1 if either the
connection handshake or the message exchange doesn't complete.

### Files

| Path | Action | Purpose |
|------|--------|---------|
| `vcpkg.json` | new | Manifest: declares the GNS dep |
| `vcpkg-configuration.json` | new | Pinned vcpkg registry baseline |
| `CMakeLists.txt` | modify | `find_package(GameNetworkingSockets CONFIG REQUIRED)` and `add_subdirectory(games/04-net-pingpong)` |
| `games/04-net-pingpong/CMakeLists.txt` | new | exe target, links `GameNetworkingSockets::GameNetworkingSockets` |
| `games/04-net-pingpong/main.cpp` | new | The smoke test |
| `.github/workflows/ci.yml` | modify | vcpkg bootstrap + binary cache + run smoke test |
| `docs/engine/build-setup.md` | new | One-page vcpkg setup instructions |

## Testing

The exe IS the test. Its exit code is the verdict: 0 = success, non-zero =
failure (with a descriptive line on stderr).

No CTest binary, no unit tests for GNS itself. Reasoning:
- The exe is the smallest end-to-end proof we can run.
- A unit test of "GNS sends a message" would be a meaningless mock or would
  duplicate the exe.
- Future milestones (M8.1+) will introduce `iron::NetTransport` which IS
  unit-testable (mock transport, deterministic message dispatch).

CI runs the exe as one workflow step after the build.

## Risks

- **vcpkg first-build CI time.** Cold-cache CI run rebuilds GNS + libsodium
  + protobuf + abseil — could push CI from ~3 min to ~15 min the first time.
  vcpkg binary cache (via `actions/cache`) mitigates: keyed on `vcpkg.json`
  SHA so re-runs are cache hits. Worst case: contributors wait once when
  the dep file changes.
- **MSVC `/W4 /permissive-` vs GNS headers.** The project compiles with
  strict warnings. GNS public headers may emit warnings under that regime;
  if so, add a per-target `target_compile_options(net-pingpong PRIVATE /W3)`
  or wrap the GNS include in `#pragma warning(push/pop)`. Localize the
  exception — don't relax warnings for the whole project.
- **vcpkg learning curve.** First time using vcpkg on this project. Mostly
  one-time cost; future deps become a one-line `vcpkg.json` change.
- **GNS C-style callback ergonomics.** The file-scope pointer pattern is
  ugly. Acceptable in a 150-line smoke test; M8.1 will wrap this properly
  in `iron::NetTransport`.

## Out of scope

- `iron::NetTransport` / engine networking abstraction (M8.1)
- `iron::Replicated<T>` or "network variables" (M8.2+)
- Snapshot interpolation, lockstep, rollback — protocol architecture (M8.2+)
- Steam Datagram Relay, Steam account/auth, matchmaking (much later)
- Two-process or cross-machine smoke testing (later, alongside protocol work)
- NAT traversal (free with SDR; not relevant without it)
- Game integration (multiplayer Strandbound is the eventual destination,
  many milestones away)
