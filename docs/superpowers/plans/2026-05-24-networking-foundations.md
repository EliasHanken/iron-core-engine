# Networking Foundations (M8.0) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring Valve's GameNetworkingSockets (GNS) into the build via vcpkg, ship a `games/04-net-pingpong` exe that performs a single-process listen+connect+ping+pong on localhost and returns exit code 0, wire CI to run it.

**Architecture:** vcpkg manifest mode declares the GNS dep (with libsodium crypto). CMake `find_package(GameNetworkingSockets CONFIG REQUIRED)` picks it up. A new exe under `games/04-net-pingpong/` exercises the GNS C++ API directly — no engine abstraction (that's M8.1+). CI gains a vcpkg bootstrap step + binary-cache step before the existing configure step.

**Tech Stack:** vcpkg (manifest mode), CMake 3.20+, MSVC `/std:c++latest`, GameNetworkingSockets, libsodium (transitive), protobuf (transitive), abseil (transitive).

**Spec:** `docs/superpowers/specs/2026-05-24-networking-foundations-design.md`

---

## File Structure

**New files:**
- `vcpkg.json` — manifest declaring the `gamenetworkingsockets` dep
- `vcpkg-configuration.json` — pinned vcpkg registry baseline SHA
- `games/04-net-pingpong/CMakeLists.txt` — exe target; links `GameNetworkingSockets::GameNetworkingSockets`
- `games/04-net-pingpong/main.cpp` — the smoke test (~150 lines)
- `docs/engine/build-setup.md` — vcpkg bootstrap instructions for new contributors

**Modified files:**
- `CMakeLists.txt` (top-level) — `find_package(GameNetworkingSockets CONFIG REQUIRED)` + `add_subdirectory(games/04-net-pingpong)`
- `.github/workflows/ci.yml` — vcpkg bootstrap + cache + smoke-test step

---

## Task 0: Branch setup

**Files:** none (git state only)

`main` is protected — work goes on a feature branch.

- [ ] **Step 1: Create and switch to the feature branch**

```powershell
git checkout -b feat/networking-foundations
git status
```

Expected: `On branch feat/networking-foundations`, clean tree.

---

## Task 1: vcpkg manifest + build-setup doc

**Files:**
- Create: `vcpkg.json`
- Create: `vcpkg-configuration.json`
- Create: `docs/engine/build-setup.md`

No CMake changes yet — those land in Task 2. This task just declares the dep and documents the local bootstrap.

- [ ] **Step 1: Create `vcpkg.json`**

```json
{
  "name": "iron-core-engine",
  "version-string": "0.0.0",
  "dependencies": [
    "gamenetworkingsockets"
  ]
}
```

- [ ] **Step 2: Choose a vcpkg baseline SHA**

The baseline pins which version of every vcpkg port we resolve to. Pick a recent commit from https://github.com/microsoft/vcpkg/commits/master and copy its full SHA. (Alternatively, after a local vcpkg clone, run `vcpkg x-update-baseline --add-initial-baseline` which writes the file for you.)

Record the SHA you chose — it goes in Step 3.

- [ ] **Step 3: Create `vcpkg-configuration.json`**

```json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "PASTE_SHA_FROM_STEP_2_HERE"
  }
}
```

Replace `PASTE_SHA_FROM_STEP_2_HERE` with the 40-character SHA from Step 2.

- [ ] **Step 4: Create `docs/engine/build-setup.md`**

```markdown
# Build setup

Iron Core Engine builds with CMake + MSVC. Most dependencies (GLFW, glad,
stb) come in via `FetchContent` and need no manual setup. Native libraries
that don't play well with `FetchContent` (currently: GameNetworkingSockets)
come in via [vcpkg](https://github.com/microsoft/vcpkg) in manifest mode.

## One-time vcpkg bootstrap

Clone vcpkg anywhere outside this repo (e.g. `C:\src\vcpkg`):

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\src\vcpkg
C:\src\vcpkg\bootstrap-vcpkg.bat
```

## Configure CMake with the vcpkg toolchain

Pass the toolchain file once at configure time:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/src/vcpkg/scripts/buildsystems/vcpkg.cmake
```

On first configure vcpkg will download and build GameNetworkingSockets and
its transitive deps (libsodium, protobuf, abseil). This takes ~5–10 minutes
on a cold cache. Subsequent configures take seconds — vcpkg caches built
packages in `%LOCALAPPDATA%/vcpkg/`.

After configure, build and test as usual:

```powershell
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

## Updating the dependency baseline

`vcpkg-configuration.json` pins a specific vcpkg registry commit so every
machine resolves to the same package versions. To bump it (e.g. to pick up a
new GNS release), edit the `baseline` field to a newer SHA from
https://github.com/microsoft/vcpkg/commits/master and re-configure.
```

- [ ] **Step 5: Commit**

```powershell
git add vcpkg.json vcpkg-configuration.json docs/engine/build-setup.md
git commit -m "Build: vcpkg manifest for GameNetworkingSockets" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: Wire top-level CMake to find GNS

**Files:**
- Modify: `CMakeLists.txt`

Adds `find_package(GameNetworkingSockets CONFIG REQUIRED)` so other targets can link `GameNetworkingSockets::GameNetworkingSockets`. **`ironcore` is not linked to GNS** in this milestone — only the new ping-pong exe will be.

- [ ] **Step 1: Edit `CMakeLists.txt`**

Use the Edit tool. Find the line `FetchContent_MakeAvailable(glfw glad)` (currently line 38) and add this immediately after the glad-runtime-library block (i.e., after the `endif()` that closes the `if(MSVC)` glad fixup, currently around line 51, before `add_subdirectory(third_party)` on line 53):

```cmake
# --- GameNetworkingSockets: UDP transport with reliable channels, encryption,
#     congestion control. Brought in via vcpkg manifest mode; see
#     docs/engine/build-setup.md for the one-time vcpkg toolchain setup.
find_package(GameNetworkingSockets CONFIG REQUIRED)
```

The placement (after FetchContent deps, before our own subdirectories) keeps related external-dep wiring together.

- [ ] **Step 2: Re-configure CMake**

You must have vcpkg installed and pass the toolchain file (see `docs/engine/build-setup.md`). Example:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/src/vcpkg/scripts/buildsystems/vcpkg.cmake
```

First run will download + build GNS + libsodium + protobuf + abseil. Expect 5–10 minutes. Subsequent configures take seconds.

Expected output near the end of configure:
```
-- Found GameNetworkingSockets: ...
-- Configuring done
-- Generating done
```

If `find_package` fails, double-check that `-DCMAKE_TOOLCHAIN_FILE=...` was passed and that `vcpkg.json` is at the repo root.

- [ ] **Step 3: Build existing targets to confirm nothing broke**

```powershell
cmake --build build
```

Expected: all existing targets (`ironcore`, `strandbound`, `showcase`, `spinning-cube`, all tests) build cleanly. No code links GNS yet, so adding `find_package` should be a no-op for them.

- [ ] **Step 4: Run existing test suite**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 16/16 tests pass (same as before this milestone).

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt
git commit -m "Build: find_package(GameNetworkingSockets) at top level" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: The ping-pong exe

**Files:**
- Create: `games/04-net-pingpong/CMakeLists.txt`
- Create: `games/04-net-pingpong/main.cpp`
- Modify: `CMakeLists.txt` (add `add_subdirectory(games/04-net-pingpong)`)

This is the headline deliverable. Single process, one listen socket, one client connection, one PING + one PONG, exit 0.

- [ ] **Step 1: Create the per-game CMakeLists**

Create `games/04-net-pingpong/CMakeLists.txt`:

```cmake
add_executable(net-pingpong main.cpp)
target_link_libraries(net-pingpong PRIVATE
  GameNetworkingSockets::GameNetworkingSockets)
```

Notes:
- No `ironcore` link — this exe deliberately does not depend on the engine library (the engine has no networking abstraction yet).
- No asset-copy step — the exe takes no command-line args and reads no files.

- [ ] **Step 2: Register the exe**

Use the Edit tool on the top-level `CMakeLists.txt`. After `add_subdirectory(games/03-showcase)` (the line added in milestone 7) add:

```cmake
add_subdirectory(games/04-net-pingpong)
```

- [ ] **Step 3: Create the smoke-test main**

Create `games/04-net-pingpong/main.cpp`. The full source follows. It uses GNS's C-style status-change callback via a file-scope state pointer (matches Valve's official pattern; see the `Example_ChatClient` in the GNS repo).

```cpp
// Iron Core Engine — networking smoke test (M8.0).
//
// Single process, two GNS endpoints on the same localhost UDP port. The
// client connects to the listener, sends "PING" reliably, the listener
// replies "PONG", both messages are verified, and the program prints "OK"
// and exits with code 0.
//
// Failure modes (all exit code 1):
//   - GameNetworkingSockets_Init fails
//   - listen/connect API returns an invalid handle
//   - either side fails to reach kESteamNetworkingConnectionState_Connected within 2 s
//   - PING or PONG mismatch
//   - any other 2 s wall-clock timeout during the exchange

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

struct PingPongState {
    ISteamNetworkingSockets* sockets = nullptr;
    HSteamListenSocket       listenSocket  = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup       listenerPollGroup = k_HSteamNetPollGroup_Invalid;
    HSteamNetConnection      clientConn  = k_HSteamNetConnection_Invalid;  // client -> server
    HSteamNetConnection      acceptedConn = k_HSteamNetConnection_Invalid; // server-side handle
    bool                     clientConnected = false;
    bool                     serverAcceptedClient = false;
    bool                     pingReceivedByServer = false;
    bool                     pongReceivedByClient = false;
    bool                     fatal = false;
    std::string              failReason;
};

PingPongState* g_state = nullptr;

void fail(const char* reason) {
    if (!g_state->fatal) {
        g_state->fatal = true;
        g_state->failReason = reason;
    }
    std::fprintf(stderr, "net-pingpong: %s\n", reason);
}

// Status-change callback. GNS C-style callback dispatches to file-scope state.
void onStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
    PingPongState& s = *g_state;
    const HSteamNetConnection h = info->m_hConn;
    const ESteamNetworkingConnectionState state = info->m_info.m_eState;

    switch (state) {
        case k_ESteamNetworkingConnectionState_Connecting:
            // Server side: a new client is asking to connect. Accept it and
            // bind it to our poll group so we can dequeue its messages.
            if (h != s.clientConn) {
                if (s.sockets->AcceptConnection(h) != k_EResultOK) {
                    fail("AcceptConnection failed");
                    return;
                }
                if (!s.sockets->SetConnectionPollGroup(h, s.listenerPollGroup)) {
                    fail("SetConnectionPollGroup failed");
                    return;
                }
                s.acceptedConn = h;
                s.serverAcceptedClient = true;
            }
            break;

        case k_ESteamNetworkingConnectionState_Connected:
            if (h == s.clientConn) {
                s.clientConnected = true;
            }
            break;

        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
            fail("connection closed unexpectedly");
            s.sockets->CloseConnection(h, 0, nullptr, false);
            break;

        default:
            break;
    }
}

bool sendReliable(HSteamNetConnection conn, const char* msg) {
    EResult r = g_state->sockets->SendMessageToConnection(
        conn, msg, static_cast<uint32>(std::strlen(msg)),
        k_nSteamNetworkingSend_Reliable, nullptr);
    return r == k_EResultOK;
}

// Returns true if a message arrived and matched expected; false on no
// message; sets fatal if a message arrived but didn't match.
bool tryReceive(HSteamNetConnection conn, const char* expected) {
    SteamNetworkingMessage_t* msg = nullptr;
    int n = g_state->sockets->ReceiveMessagesOnConnection(conn, &msg, 1);
    if (n <= 0) return false;
    const bool ok = msg->GetSize() == std::strlen(expected) &&
                    std::memcmp(msg->GetData(), expected, msg->GetSize()) == 0;
    msg->Release();
    if (!ok) {
        fail("message payload mismatch");
        return false;
    }
    return true;
}

bool tryReceiveOnPollGroup(HSteamNetPollGroup pg, const char* expected) {
    SteamNetworkingMessage_t* msg = nullptr;
    int n = g_state->sockets->ReceiveMessagesOnPollGroup(pg, &msg, 1);
    if (n <= 0) return false;
    const bool ok = msg->GetSize() == std::strlen(expected) &&
                    std::memcmp(msg->GetData(), expected, msg->GetSize()) == 0;
    msg->Release();
    if (!ok) {
        fail("message payload mismatch");
        return false;
    }
    return true;
}

}  // namespace

int main() {
    SteamNetworkingErrMsg errMsg{};
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        std::fprintf(stderr, "net-pingpong: GameNetworkingSockets_Init failed: %s\n", errMsg);
        return 1;
    }

    PingPongState state;
    g_state = &state;
    state.sockets = SteamNetworkingSockets();

    SteamNetworkingIPAddr serverAddr;
    serverAddr.Clear();
    serverAddr.SetIPv4(0x7F000001, 27015);  // 127.0.0.1:27015

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(onStatusChanged));

    state.listenSocket = state.sockets->CreateListenSocketIP(serverAddr, 1, &opt);
    if (state.listenSocket == k_HSteamListenSocket_Invalid) {
        std::fprintf(stderr, "net-pingpong: CreateListenSocketIP failed\n");
        GameNetworkingSockets_Kill();
        return 1;
    }
    state.listenerPollGroup = state.sockets->CreatePollGroup();
    if (state.listenerPollGroup == k_HSteamNetPollGroup_Invalid) {
        std::fprintf(stderr, "net-pingpong: CreatePollGroup failed\n");
        state.sockets->CloseListenSocket(state.listenSocket);
        GameNetworkingSockets_Kill();
        return 1;
    }

    state.clientConn = state.sockets->ConnectByIPAddress(serverAddr, 1, &opt);
    if (state.clientConn == k_HSteamNetConnection_Invalid) {
        std::fprintf(stderr, "net-pingpong: ConnectByIPAddress failed\n");
        state.sockets->DestroyPollGroup(state.listenerPollGroup);
        state.sockets->CloseListenSocket(state.listenSocket);
        GameNetworkingSockets_Kill();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool pingSent = false;
    bool pongSent = false;

    while (!state.fatal && std::chrono::steady_clock::now() < deadline) {
        state.sockets->RunCallbacks();

        // Once both sides are connected, client sends PING.
        if (state.clientConnected && state.serverAcceptedClient && !pingSent) {
            if (!sendReliable(state.clientConn, "PING")) {
                fail("client SendMessageToConnection PING failed");
                break;
            }
            pingSent = true;
        }

        // Server polls its poll group for PING and replies PONG.
        if (pingSent && !state.pingReceivedByServer) {
            if (tryReceiveOnPollGroup(state.listenerPollGroup, "PING")) {
                state.pingReceivedByServer = true;
            }
        }
        if (state.pingReceivedByServer && !pongSent) {
            if (!sendReliable(state.acceptedConn, "PONG")) {
                fail("server SendMessageToConnection PONG failed");
                break;
            }
            pongSent = true;
        }

        // Client polls its connection directly for PONG.
        if (pongSent && !state.pongReceivedByClient) {
            if (tryReceive(state.clientConn, "PONG")) {
                state.pongReceivedByClient = true;
            }
        }

        if (state.pongReceivedByClient) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const bool success = state.pongReceivedByClient && !state.fatal;
    if (!success && !state.fatal) {
        fail("timeout waiting for ping-pong exchange");
    }

    // Cleanup (best-effort; ignore failures during shutdown).
    if (state.clientConn != k_HSteamNetConnection_Invalid) {
        state.sockets->CloseConnection(state.clientConn, 0, "bye", false);
    }
    if (state.acceptedConn != k_HSteamNetConnection_Invalid) {
        state.sockets->CloseConnection(state.acceptedConn, 0, "bye", false);
    }
    if (state.listenerPollGroup != k_HSteamNetPollGroup_Invalid) {
        state.sockets->DestroyPollGroup(state.listenerPollGroup);
    }
    if (state.listenSocket != k_HSteamListenSocket_Invalid) {
        state.sockets->CloseListenSocket(state.listenSocket);
    }
    GameNetworkingSockets_Kill();
    g_state = nullptr;

    if (success) {
        std::printf("OK\n");
        return 0;
    }
    return 1;
}
```

NOTES for the implementer (don't include these in the file):
- GNS public headers live under `<steam/...>` (a Steam-ism even in the standalone build). The three includes at the top are the standard set.
- `uint32` is a GNS-internal typedef pulled in via the `steam/...` headers — that's why it works without a `<cstdint>` cast.
- `HSteamListenSocket` and `HSteamNetConnection` are unsigned int handles; `k_..._Invalid` sentinels are defined in the GNS headers.
- The `1` passed to `CreateListenSocketIP`/`ConnectByIPAddress` is `nOptions` — the number of `SteamNetworkingConfigValue_t` entries pointed to by `opt`. We pass one option (the callback).
- A 2 second deadline is generous for localhost; GNS handshake on loopback typically completes in <10 ms.

- [ ] **Step 4: Build the exe**

```powershell
cmake --build build --target net-pingpong
```

Expected: clean build. If MSVC warns under `/W4 /permissive-` from GNS headers, add a per-target relaxation in `games/04-net-pingpong/CMakeLists.txt`:

```cmake
if(MSVC)
  target_compile_options(net-pingpong PRIVATE /W3)
endif()
```

Don't relax warnings project-wide — keep the exception localized.

- [ ] **Step 5: Run the smoke test**

```powershell
./build/games/04-net-pingpong/Debug/net-pingpong.exe
echo "exit code: $LASTEXITCODE"
```

Expected:
```
OK
exit code: 0
```

If you see a non-zero exit code, read the stderr line for the diagnostic. Most likely failures:
- Port 27015 already in use on your machine → pick a different port in the source (search `27015`) and rerun.
- Firewall blocking loopback (rare on Windows) → tell the user.

- [ ] **Step 6: Commit**

```powershell
git add games/04-net-pingpong CMakeLists.txt
git commit -m "Networking: ping-pong smoke test exe (single-process GNS)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 4: CI vcpkg bootstrap + smoke test

**Files:**
- Modify: `.github/workflows/ci.yml`

Two changes:
1. Bootstrap vcpkg before CMake configure, with binary-caching so subsequent runs aren't slow.
2. Run `net-pingpong.exe` after the build and fail CI on non-zero exit.

- [ ] **Step 1: Edit `.github/workflows/ci.yml`**

Use the Edit tool. Replace the whole `steps:` block in the `build-and-test` job with the following. This adds (a) a vcpkg setup step, (b) a binary-cache step, (c) the toolchain file flag on configure, and (d) a new "Smoke test: net-pingpong" step after Build.

```yaml
    steps:
      - name: Checkout
        uses: actions/checkout@v4
        with:
          lfs: true

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: '3.x'

      - name: Install glad generator dependency
        run: pip install jinja2

      - name: Setup vcpkg
        uses: lukka/run-vcpkg@v11
        with:
          vcpkgGitCommitId: '<PASTE_THE_SAME_BASELINE_SHA_AS_VCPKG_CONFIGURATION_JSON>'

      - name: Configure
        run: cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=${env:VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake

      - name: Build
        run: cmake --build build

      - name: Smoke test - net-pingpong
        run: |
          ./build/games/04-net-pingpong/Debug/net-pingpong.exe
          if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

      - name: Test
        run: ctest --test-dir build -C Debug --output-on-failure
```

Replace `<PASTE_THE_SAME_BASELINE_SHA_AS_VCPKG_CONFIGURATION_JSON>` with the exact SHA from `vcpkg-configuration.json`'s `baseline` field (Task 1, Step 2). The two must match — otherwise the CI vcpkg checkout will resolve a different version of GNS than your local one.

Why `lukka/run-vcpkg@v11`? It's the most widely used vcpkg GitHub Action; it handles the bootstrap + binary cache setup transparently. It sets `VCPKG_ROOT` env var that we reference in the Configure step.

The binary cache is automatic with `lukka/run-vcpkg@v11` when it detects manifest mode (it caches to GitHub Actions cache, keyed on `vcpkg.json` SHA). First run on a clean cache: ~10–15 min. Subsequent runs: cache hit, GNS build skipped, total CI time back near baseline.

- [ ] **Step 2: Commit**

```powershell
git add .github/workflows/ci.yml
git commit -m "CI: bootstrap vcpkg, build GNS, run net-pingpong smoke test" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

- [ ] **Step 3: Push and verify CI runs**

```powershell
git push -u origin feat/networking-foundations
```

Open the resulting Actions run on GitHub (URL printed by `git push`). The first run will spend ~10 min in "Setup vcpkg" building GNS from source. Expected sequence:
1. Checkout — fast
2. Set up Python — fast
3. Install glad generator dependency — fast
4. Setup vcpkg — **slow on first run** (builds GNS + libsodium + protobuf + abseil)
5. Configure — fast (uses the pre-built vcpkg packages)
6. Build — same as before plus the ~150 line net-pingpong exe
7. Smoke test - net-pingpong — should print `OK`, exit 0
8. Test — 16/16 existing tests pass

If the Setup vcpkg step fails, check the action logs for which dep failed to build. The most common failure is a transient network issue — retry first.

If the Smoke test step fails, read the stderr in the action log; the message identifies which GNS API call failed.

---

## Task 5: Code review pass + PR

**Files:** none modified — read-only review

Per the standing memory `always-code-review-changes`, every code change gets a review pass.

- [ ] **Step 1: Show the diff range**

```powershell
git log --oneline main..HEAD
git diff main --stat
```

Expected: 4 commits (Tasks 1–4), files touched as listed in the File Structure section.

- [ ] **Step 2: Dispatch a code-quality review agent**

Dispatch `feature-dev:code-reviewer` (or `general-purpose` if unavailable) with this prompt:

> Review the M8.0 networking-foundations changes (`git diff main`) in the Iron Core Engine. Focus on: (1) `vcpkg.json` + `vcpkg-configuration.json` correctness (does the baseline SHA match between the two files? is the manifest schema correct?); (2) `CMakeLists.txt` placement of `find_package` (correct ordering, no transitive deps leaked to `ironcore`); (3) `games/04-net-pingpong/main.cpp` correctness (handle invalidation on every error path, no resource leaks in the cleanup tail, the file-scope state pointer is set and cleared correctly, the 2 s deadline can't be defeated by `sleep_for` overshoot, integer types match GNS API signatures); (4) `.github/workflows/ci.yml` baseline SHA matches `vcpkg-configuration.json`. Report high-confidence correctness issues, missed engine conventions, resource leaks, mistaken assumptions about the GNS API. Skip style nits. Cap at 10 findings. Under 400 words.

- [ ] **Step 3: Address findings**

Apply fixes. Per `feedback-code-quicker`, push back on cosmetic suggestions; only block on real correctness issues.

- [ ] **Step 4: Build + smoke test one more time**

```powershell
cmake --build build
./build/games/04-net-pingpong/Debug/net-pingpong.exe
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, smoke test prints `OK`, all 16 existing tests still pass.

- [ ] **Step 5: Commit any review fixes (skip if no fixes needed)**

```powershell
git add -A
git commit -m "Networking: address M8.0 code-review findings" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

- [ ] **Step 6: Open the PR**

```powershell
git push
gh pr create --title "Milestone 8.0: Networking foundations (GNS build + smoke test)" --body "$(@'
## Summary
- Adds vcpkg manifest (`vcpkg.json` + `vcpkg-configuration.json`) declaring `gamenetworkingsockets`.
- New `games/04-net-pingpong` single-process exe: opens a listen socket, connects to itself, exchanges PING/PONG reliably, prints `OK`, exits 0.
- Top-level `CMakeLists.txt` gains `find_package(GameNetworkingSockets CONFIG REQUIRED)`. `ironcore` is deliberately NOT linked to GNS — that's M8.1.
- CI workflow now bootstraps vcpkg with binary caching and runs the smoke test as a build step.
- New `docs/engine/build-setup.md` explains the one-time vcpkg setup.

## Test plan
- [x] `cmake --build build --target net-pingpong` produces the exe
- [x] Running the exe prints `OK` and exits 0
- [x] All 16 existing tests still pass
- [x] CI green (first run is slow due to vcpkg cold cache; subsequent runs cached)

## Out of scope (M8.1+)
- Engine abstraction (`iron::NetTransport`)
- Two-process / cross-machine smoke test
- Protocol design, snapshot/state sync
- Steam Datagram Relay, Steam auth
- Game integration

🤖 Generated with [Claude Code](https://claude.com/claude-code)
'@ | Out-String)"
```

NOTE: the heredoc above uses PowerShell here-string syntax (`@'...'@`). Cross-check the exact syntax `gh pr create` wants on your shell; if it has trouble, fall back to a single `--body-file pr-body.md` flag.

---

## Self-review (run after writing the plan, before handoff)

Already done inline:
- **Spec coverage:**
  - vcpkg.json / vcpkg-configuration.json → Task 1
  - `find_package(GameNetworkingSockets CONFIG REQUIRED)` at top level → Task 2
  - `games/04-net-pingpong/{CMakeLists.txt,main.cpp}` → Task 3
  - `docs/engine/build-setup.md` → Task 1
  - CI vcpkg bootstrap + binary cache + smoke-test step → Task 4
  - 2-second timeout, exit codes 0/1, `OK` on success → Task 3 (in the source)
  - "No engine integration" / "No CTest binary" → respected (no engine touch; no ctest entry for ping-pong)
  - All Non-goals in the spec are observed in the plan (no two-process test, no protocol design, no `iron::NetTransport`, no Steam, no threading).
- **Placeholder scan:** the only `<PASTE_...>` token in the plan is the explicit "you must look this up at runtime" instruction for the vcpkg baseline SHA — same in both `vcpkg-configuration.json` and `ci.yml`, and the plan explicitly tells the implementer the two must be identical. Not a plan failure.
- **Type consistency:**
  - `GameNetworkingSockets::GameNetworkingSockets` is the target name used in both Task 3 (target_link_libraries) and the file-structure summary.
  - The port (27015) is the only literal — same value in source and the troubleshooting note.
  - `net-pingpong` exe name is consistent across CMake target, `add_subdirectory` argument, and CI invocation.
