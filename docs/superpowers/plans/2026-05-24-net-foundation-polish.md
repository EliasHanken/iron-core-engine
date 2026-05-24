# Networking Foundation Polish (M8.4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add four engine primitives that unblock M8.5/M8.6: `iron::TimeHistory<T>` (time-stamped buffer with interpolation query), `iron::FixedTickScheduler` (accumulator-pattern timestep helper), `iron::ConnectionStats` + `GnsTransport::stats()` (live network health), and `iron::Hud::addNetworkStatsWidget(...)` (debug HUD line). Apply these to net-cubes and net-tag for smooth remote-player display, add a 4-byte game-id handshake to both games to prevent the cross-connect footgun, and use `FixedTickScheduler` to clean up net-tag's score-broadcast cadence.

**Architecture:** Engine helpers are mostly header-only templates (TimeHistory, FixedTickScheduler) so no separate compilation unit is needed. ConnectionStats is a plain struct + one new const method on GnsTransport (`stats(ConnectionId)`) that wraps `ISteamNetworkingSockets::GetConnectionRealTimeStatus`. The HUD widget is a thin convenience over existing `addText`/`setText` methods. Games adopt all four with surgical edits — no game-level refactor of position-broadcast logic, just a swap from `unordered_map<peerId, CubeState>` + manual lerp to `unordered_map<peerId, TimeHistory<Vec3>>` + `sampleAtDelay(100ms)`.

**Tech Stack:** C++23 templates, `std::chrono::steady_clock`, `std::optional`, `std::deque`. Custom CTest harness for unit tests. GameNetworkingSockets `GetConnectionRealTimeStatus` API.

**Spec:** `docs/superpowers/specs/2026-05-24-net-foundation-polish-design.md`

---

## File Structure

**New files:**
- `engine/net/TimeHistory.h` — header-only template
- `engine/core/FixedTickScheduler.h` — header-only
- `engine/net/NetworkStats.h` — `iron::ConnectionStats` POD struct
- `tests/test_time_history.cpp` — 6 cases against Vec3
- `tests/test_fixed_tick_scheduler.cpp` — 4 cases

**Modified files:**
- `engine/math/Vec.h` — add free function `interpolate(Vec3, Vec3, float)`
- `engine/net/GnsTransport.h` — add `ConnectionStats stats(ConnectionId) const`
- `engine/net/GnsTransport.cpp` — implement `stats(...)`
- `engine/ui/Hud.h` — add `NetStatsHudHandle`, `addNetworkStatsWidget`, `updateNetworkStats`
- `engine/ui/Hud.cpp` — implement the two new methods
- `games/05-net-cubes/Messages.h` — add `kGameId` constant + `gameId` field on `HelloMsg`
- `games/05-net-cubes/main.cpp` — gameId reject, TimeHistory refactor, net-stats HUD
- `games/06-net-tag/Messages.h` — same Messages.h changes
- `games/06-net-tag/main.cpp` — same main.cpp changes + adopt `FixedTickScheduler` for score broadcast cadence
- `tests/CMakeLists.txt` — register two new tests
- `docs/engine/networking.md` — add TimeHistory + game-id + debug HUD sections

---

## Task 0: Branch setup

**Files:** none (git state only)

`main` is protected — work goes on a feature branch.

- [ ] **Step 1: Create and switch to the feature branch**

```powershell
git checkout -b feat/net-foundation-polish
git status
```

Expected: `On branch feat/net-foundation-polish`. CRLF/LF warnings on docs files are harmless background noise.

---

## Task 1: `iron::interpolate(Vec3)` free function (TDD)

**Files:**
- Modify: `engine/math/Vec.h`
- Modify: `tests/test_vec.cpp` (existing) — add cases for `interpolate`

Tiny atomic addition. `TimeHistory<T>` (Task 2) will require this via ADL.

- [ ] **Step 1: Write failing tests in `tests/test_vec.cpp`**

Find a good insertion point near the end of `main()` (before the `return iron_test_result();` line) and add this block:

```cpp
    // interpolate(Vec3, Vec3, float) — used by TimeHistory<Vec3>.
    {
        const iron::Vec3 a{0.0f, 0.0f, 0.0f};
        const iron::Vec3 b{10.0f, 20.0f, -4.0f};
        // t=0 returns a
        {
            const iron::Vec3 r = iron::interpolate(a, b, 0.0f);
            CHECK_NEAR(r.x, 0.0f); CHECK_NEAR(r.y, 0.0f); CHECK_NEAR(r.z, 0.0f);
        }
        // t=1 returns b
        {
            const iron::Vec3 r = iron::interpolate(a, b, 1.0f);
            CHECK_NEAR(r.x, 10.0f); CHECK_NEAR(r.y, 20.0f); CHECK_NEAR(r.z, -4.0f);
        }
        // t=0.5 returns midpoint
        {
            const iron::Vec3 r = iron::interpolate(a, b, 0.5f);
            CHECK_NEAR(r.x, 5.0f); CHECK_NEAR(r.y, 10.0f); CHECK_NEAR(r.z, -2.0f);
        }
        // t=0.25 returns quarter
        {
            const iron::Vec3 r = iron::interpolate(a, b, 0.25f);
            CHECK_NEAR(r.x, 2.5f); CHECK_NEAR(r.y, 5.0f); CHECK_NEAR(r.z, -1.0f);
        }
    }
```

- [ ] **Step 2: Verify the test fails to compile**

```powershell
cmake --build build --target test_vec
```

Expected: `'iron::interpolate': identifier not found` or similar.

- [ ] **Step 3: Add `interpolate` to `engine/math/Vec.h`**

Find an existing free function in the `iron` namespace (e.g. `normalize` or `cross` for Vec3) and add this AFTER it inside the `namespace iron { ... }` block:

```cpp
// Linear interpolation between two Vec3 by parameter t.
// t=0 returns a, t=1 returns b. No clamping — callers may pass
// values outside [0,1] for extrapolation if they really mean it.
inline Vec3 interpolate(Vec3 a, Vec3 b, float t) {
    return Vec3{
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
    };
}
```

- [ ] **Step 4: Build + run the test**

```powershell
cmake --build build --target test_vec
ctest --test-dir build -C Debug -R test_vec --output-on-failure
```

Use `timeout: 120000`. Expected: `OK - all checks passed`, CTest PASS.

- [ ] **Step 5: Commit**

```powershell
git add engine/math/Vec.h tests/test_vec.cpp
git commit -m "Math: interpolate(Vec3, Vec3, float) free function" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: `iron::TimeHistory<T>` (TDD)

**Files:**
- Create: `engine/net/TimeHistory.h`
- Create: `tests/test_time_history.cpp`
- Modify: `tests/CMakeLists.txt`

Header-only template. No engine `.cpp` is needed.

- [ ] **Step 1: Write failing test `tests/test_time_history.cpp`**

```cpp
#include "test_framework.h"
#include "math/Vec.h"
#include "net/TimeHistory.h"

#include <chrono>
#include <optional>

using namespace iron;
using namespace std::chrono_literals;

namespace {
// Deterministic test clock origin so we don't depend on real wall time.
TimeHistory<Vec3>::TimePoint t0() {
    return TimeHistory<Vec3>::TimePoint{};
}
}  // namespace

int main() {
    // Empty buffer → sample returns nullopt.
    {
        TimeHistory<Vec3> h;
        CHECK(!h.sample(t0()).has_value());
        CHECK(h.size() == 0);
    }

    // Single sample → sample(any time) returns it.
    {
        TimeHistory<Vec3> h;
        h.push(t0() + 1000ms, Vec3{1, 2, 3});
        CHECK(h.size() == 1);
        const auto a = h.sample(t0() + 1000ms);
        CHECK(a.has_value());
        CHECK_NEAR(a->x, 1.0f); CHECK_NEAR(a->y, 2.0f); CHECK_NEAR(a->z, 3.0f);
        // Query in the future (after the sample): still returns it (no extrapolation).
        const auto b = h.sample(t0() + 5000ms);
        CHECK(b.has_value());
        CHECK_NEAR(b->x, 1.0f);
        // Query in the past (before the sample): also returns it.
        const auto c = h.sample(t0() + 500ms);
        CHECK(c.has_value());
        CHECK_NEAR(c->x, 1.0f);
    }

    // Two samples → query between them interpolates linearly.
    {
        TimeHistory<Vec3> h;
        h.push(t0() + 1000ms, Vec3{0, 0, 0});
        h.push(t0() + 2000ms, Vec3{10, 0, 0});
        const auto mid = h.sample(t0() + 1500ms);
        CHECK(mid.has_value());
        CHECK_NEAR(mid->x, 5.0f); CHECK_NEAR(mid->y, 0.0f); CHECK_NEAR(mid->z, 0.0f);
        // Quarter
        const auto q = h.sample(t0() + 1250ms);
        CHECK(q.has_value());
        CHECK_NEAR(q->x, 2.5f);
        // Three quarter
        const auto tq = h.sample(t0() + 1750ms);
        CHECK(tq.has_value());
        CHECK_NEAR(tq->x, 7.5f);
    }

    // Query before earliest → returns earliest sample (no back-extrapolation).
    {
        TimeHistory<Vec3> h;
        h.push(t0() + 1000ms, Vec3{0, 0, 0});
        h.push(t0() + 2000ms, Vec3{10, 0, 0});
        const auto r = h.sample(t0() + 500ms);
        CHECK(r.has_value());
        CHECK_NEAR(r->x, 0.0f);
    }

    // Query after latest → returns latest sample (no forward-extrapolation).
    {
        TimeHistory<Vec3> h;
        h.push(t0() + 1000ms, Vec3{0, 0, 0});
        h.push(t0() + 2000ms, Vec3{10, 0, 0});
        const auto r = h.sample(t0() + 5000ms);
        CHECK(r.has_value());
        CHECK_NEAR(r->x, 10.0f);
    }

    // Eviction: samples older than retention dropped on push.
    {
        // 500ms retention. Push at t=0, t=200, t=400; then push at t=1000
        // which should evict t=0 (older than 1000-500=500ms cutoff).
        TimeHistory<Vec3> h{500ms};
        h.push(t0() + 0ms,    Vec3{1, 0, 0});
        h.push(t0() + 200ms,  Vec3{2, 0, 0});
        h.push(t0() + 400ms,  Vec3{3, 0, 0});
        h.push(t0() + 1000ms, Vec3{9, 0, 0});  // triggers eviction of t=0 (and t=200; cutoff = 500)
        CHECK(h.size() == 2);  // t=400 and t=1000 remain
        // Earliest is now t=400 with value Vec3{3,...}, latest t=1000 with Vec3{9,...}.
        const auto r = h.sample(t0() + 400ms);
        CHECK(r.has_value());
        CHECK_NEAR(r->x, 3.0f);
    }

    // sampleAtDelay convenience: with a single sample at "now", a small
    // delay queries slightly in the past and still returns the sample.
    {
        TimeHistory<Vec3> h;
        const auto now = std::chrono::steady_clock::now();
        h.push(now, Vec3{7, 0, 0});
        const auto r = h.sampleAtDelay(100ms);
        CHECK(r.has_value());
        CHECK_NEAR(r->x, 7.0f);
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Verify the test fails to compile**

```powershell
cmake --build build --target test_time_history
```

Expected: header not found.

- [ ] **Step 3: Create `engine/net/TimeHistory.h`**

```cpp
#pragma once

#include "math/Vec.h"  // brings in iron::interpolate(Vec3, Vec3, float)

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace iron {

// Time-stamped sample buffer with interpolation query. Two use cases
// across the engine:
//
//   Client-side: sample(now - displayDelay) → smooth remote-peer
//   display, tolerant of jitter and modest packet loss.
//
//   Server-side: sample(client.fireTimestamp) → historical position
//   for hit-validation (lag compensation).
//
// T must support free-function `interpolate(T a, T b, float t)` found
// via ADL. For iron::Vec3 the engine provides one in math/Vec.h.
//
// Samples older than `retention` are dropped on push; default 1 second
// covers typical lag-compensation lookback comfortably.
template <typename T>
class TimeHistory {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit TimeHistory(
        std::chrono::milliseconds retention = std::chrono::milliseconds{1000})
        : retention_(retention) {}

    // Push a new sample stamped with now().
    void push(const T& sample) {
        push(Clock::now(), sample);
    }

    // Push with explicit timestamp (test-friendly).
    void push(TimePoint at, const T& sample) {
        samples_.emplace_back(at, sample);
        // Evict any samples older than the retention window relative to
        // the newest sample we just stored. Using "newest" instead of
        // Clock::now() keeps the test-friendly explicit-time API
        // deterministic regardless of wall clock.
        const TimePoint cutoff = at - retention_;
        while (!samples_.empty() && samples_.front().first < cutoff) {
            samples_.pop_front();
        }
    }

    // Sample the buffer at the given absolute time.
    // Returns nullopt if the buffer is empty.
    // Otherwise: clamps to earliest if `at` precedes it, clamps to
    // latest if `at` follows it, and linearly interpolates between
    // the two straddling samples in between.
    std::optional<T> sample(TimePoint at) const {
        if (samples_.empty()) return std::nullopt;
        if (at <= samples_.front().first) return samples_.front().second;
        if (at >= samples_.back().first)  return samples_.back().second;
        // Find the first sample with timestamp >= at.
        // (linear scan is fine — typical buffer holds < 50 samples)
        for (std::size_t i = 1; i < samples_.size(); ++i) {
            const auto& [tb, b] = samples_[i];
            if (tb >= at) {
                const auto& [ta, a] = samples_[i - 1];
                const auto span = std::chrono::duration<float>(tb - ta).count();
                if (span <= 0.0f) return a;  // identical timestamps; degenerate
                const auto offset = std::chrono::duration<float>(at - ta).count();
                const float t = offset / span;
                return interpolate(a, b, t);  // ADL
            }
        }
        return samples_.back().second;  // unreachable in practice
    }

    // Convenience: sample at `now() - displayDelay`. Common client use.
    std::optional<T> sampleAtDelay(std::chrono::milliseconds displayDelay) const {
        return sample(Clock::now() - displayDelay);
    }

    std::size_t size() const { return samples_.size(); }
    void clear() { samples_.clear(); }

private:
    std::deque<std::pair<TimePoint, T>> samples_;
    std::chrono::milliseconds retention_;
};

}  // namespace iron
```

- [ ] **Step 4: Wire test into CMake**

Edit `tests/CMakeLists.txt`. Add this line after the existing `iron_add_test(test_message_registry ...)` entry:

```cmake
iron_add_test(test_time_history test_time_history.cpp)
```

- [ ] **Step 5: Build + run the test**

```powershell
cmake --build build --target test_time_history
ctest --test-dir build -C Debug -R test_time_history --output-on-failure
```

Use `timeout: 120000`. Expected: `OK - all checks passed`, CTest PASS.

- [ ] **Step 6: Commit**

```powershell
git add engine/net/TimeHistory.h tests/test_time_history.cpp tests/CMakeLists.txt
git commit -m "Engine: TimeHistory<T> for client interpolation + server lag compensation" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: `iron::FixedTickScheduler` (TDD)

**Files:**
- Create: `engine/core/FixedTickScheduler.h`
- Create: `tests/test_fixed_tick_scheduler.cpp`
- Modify: `tests/CMakeLists.txt`

Header-only. No engine `.cpp` needed.

- [ ] **Step 1: Write failing test `tests/test_fixed_tick_scheduler.cpp`**

```cpp
#include "test_framework.h"
#include "core/FixedTickScheduler.h"

#include <chrono>

using namespace iron;
using namespace std::chrono_literals;

int main() {
    // 30 Hz scheduler (~33.333 ms / tick). Single update(0.1s) → 3 ticks.
    {
        FixedTickScheduler s{std::chrono::microseconds{33333}};
        int count = 0;
        s.update(0.1f, [&]() { ++count; });
        CHECK(count == 3);
    }

    // 30 Hz scheduler called 4 times with 0.01s each (40ms total).
    // 40ms / 33.333ms = 1 full tick + ~7ms remainder → 1 invocation total.
    {
        FixedTickScheduler s{std::chrono::microseconds{33333}};
        int count = 0;
        for (int i = 0; i < 4; ++i) s.update(0.01f, [&]() { ++count; });
        CHECK(count == 1);
    }

    // 30 Hz, dt=2.0s → 60 invocations in one update call (catches up).
    {
        FixedTickScheduler s{std::chrono::microseconds{33333}};
        int count = 0;
        s.update(2.0f, [&]() { ++count; });
        CHECK(count == 60);
    }

    // Accumulator preserves remainder across calls.
    {
        FixedTickScheduler s{std::chrono::microseconds{100000}};  // 100ms
        int count = 0;
        s.update(0.060f, [&]() { ++count; });
        CHECK(count == 0);  // 60ms < 100ms; nothing fires yet
        s.update(0.060f, [&]() { ++count; });
        CHECK(count == 1);  // 60+60 = 120ms ≥ 100ms; one tick consumed (20ms left)
        s.update(0.080f, [&]() { ++count; });
        CHECK(count == 2);  // 20+80 = 100ms; another tick consumed (0ms left)
    }

    return iron_test_result();
}
```

- [ ] **Step 2: Verify the test fails to compile**

```powershell
cmake --build build --target test_fixed_tick_scheduler
```

Expected: header not found.

- [ ] **Step 3: Create `engine/core/FixedTickScheduler.h`**

```cpp
#pragma once

#include <chrono>
#include <utility>

namespace iron {

// Accumulator-pattern fixed-timestep scheduler. Game owns one of these
// and calls `update(dtSeconds, tickFn)` once per frame. The scheduler
// invokes the callback zero or more times until the accumulated time
// has been consumed, guaranteeing deterministic tick spacing
// independent of render framerate.
//
// Typical usage:
//
//   iron::FixedTickScheduler ticker{std::chrono::milliseconds{33}};  // ~30 Hz
//   while (running) {
//       const float dt = frameDt();
//       ticker.update(dt, [&]() { simulationTick(); });
//       render();
//   }
//
// If dt is unusually large (e.g. after a process stall), `update` will
// fire `tickFn` many times in a row to catch up. Game code should treat
// this as normal — each call is just "advance the world by one
// tickInterval".
class FixedTickScheduler {
public:
    explicit FixedTickScheduler(std::chrono::microseconds tickInterval)
        : tickIntervalSec_(
              std::chrono::duration<float>(tickInterval).count()) {}

    // Advance the scheduler by `dtSeconds` and invoke `tickFn` once
    // per accumulated tickInterval.
    template <typename TickFn>
    void update(float dtSeconds, TickFn&& tickFn) {
        accumulator_ += dtSeconds;
        while (accumulator_ >= tickIntervalSec_) {
            accumulator_ -= tickIntervalSec_;
            tickFn();
        }
    }

    // Tick interval in seconds (useful when game logic needs to know
    // dt-per-tick — e.g. for physics integration step size).
    float tickIntervalSeconds() const { return tickIntervalSec_; }

private:
    float tickIntervalSec_;
    float accumulator_ = 0.0f;
};

}  // namespace iron
```

- [ ] **Step 4: Wire test into CMake**

Edit `tests/CMakeLists.txt`. Add after the `test_time_history` line from Task 2:

```cmake
iron_add_test(test_fixed_tick_scheduler test_fixed_tick_scheduler.cpp)
```

- [ ] **Step 5: Build + run the test**

```powershell
cmake --build build --target test_fixed_tick_scheduler
ctest --test-dir build -C Debug -R test_fixed_tick_scheduler --output-on-failure
```

Use `timeout: 120000`. Expected: `OK - all checks passed`, CTest PASS.

- [ ] **Step 6: Commit**

```powershell
git add engine/core/FixedTickScheduler.h tests/test_fixed_tick_scheduler.cpp tests/CMakeLists.txt
git commit -m "Engine: FixedTickScheduler (accumulator-pattern timestep helper)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 4: `iron::ConnectionStats` + `GnsTransport::stats()`

**Files:**
- Create: `engine/net/NetworkStats.h`
- Modify: `engine/net/GnsTransport.h`
- Modify: `engine/net/GnsTransport.cpp`
- Modify: `engine/CMakeLists.txt` (no — header-only ConnectionStats, no new .cpp)

Read-only accessor over GNS's `GetConnectionRealTimeStatus`. No unit
test — the values come from GNS internals; smoke tested via the HUD
in games (Task 6/7).

- [ ] **Step 1: Create `engine/net/NetworkStats.h`**

```cpp
#pragma once

#include <string>

namespace iron {

// One snapshot of a connection's network health. Populated by
// GnsTransport::stats(ConnectionId) from the underlying GNS API.
// Zero-initialised values mean "unknown / not yet measured".
struct ConnectionStats {
    float pingMs           = 0.0f;
    float packetLossPct    = 0.0f;
    float jitterMs         = 0.0f;
    float bandwidthInKbps  = 0.0f;
    float bandwidthOutKbps = 0.0f;
    // Short human-readable connection state ("Connected", "Connecting",
    // "ClosedByPeer", "ProblemDetectedLocally", or "Unknown").
    std::string state = "Unknown";
};

}  // namespace iron
```

- [ ] **Step 2: Add the `stats(ConnectionId) const` declaration to `engine/net/GnsTransport.h`**

Use the Edit tool. Find the `void poll() override;` line. After it (inside the same `public:` section), add:

```cpp

    // Live network health for one connection. Pulls from
    // ISteamNetworkingSockets::GetConnectionRealTimeStatus. Returns
    // a zero-initialised ConnectionStats with state="Unknown" if the
    // connection id is not currently tracked.
    ConnectionStats stats(ConnectionId conn) const;
```

Then add the include at the top of the file (near the other net/* includes):
```cpp
#include "net/NetworkStats.h"
```

- [ ] **Step 3: Implement `stats(...)` in `engine/net/GnsTransport.cpp`**

Add at the bottom of the file, just before the closing `} // namespace iron`:

```cpp

ConnectionStats GnsTransport::stats(ConnectionId conn) const {
    ConnectionStats out;
    if (!started_ || conn == kInvalidConnection) return out;
    auto it = idToHandle_.find(conn);
    if (it == idToHandle_.end()) return out;

    SteamNetConnectionRealTimeStatus_t rt{};
    SteamNetConnectionRealTimeLaneStatus_t lane{};
    const EResult r = sockets_->GetConnectionRealTimeStatus(
        it->second, &rt, 0, &lane);
    if (r != k_EResultOK) {
        return out;
    }

    out.pingMs           = static_cast<float>(rt.m_nPing);
    out.packetLossPct    = rt.m_flConnectionQualityRemote >= 0.0f
                              ? (1.0f - rt.m_flConnectionQualityRemote) * 100.0f
                              : 0.0f;
    out.jitterMs         = 0.0f;  // GNS does not surface jitter directly today
    out.bandwidthInKbps  = static_cast<float>(rt.m_flInBytesPerSec) * 8.0f / 1000.0f;
    out.bandwidthOutKbps = static_cast<float>(rt.m_flOutBytesPerSec) * 8.0f / 1000.0f;

    switch (rt.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
            out.state = "Connecting"; break;
        case k_ESteamNetworkingConnectionState_FindingRoute:
            out.state = "FindingRoute"; break;
        case k_ESteamNetworkingConnectionState_Connected:
            out.state = "Connected"; break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
            out.state = "ClosedByPeer"; break;
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            out.state = "ProblemDetectedLocally"; break;
        default: out.state = "Unknown"; break;
    }
    return out;
}
```

NOTE: the `idToHandle_` access in a `const` method requires `idToHandle_` to be either non-const-modified (it isn't, in this method) or for `find` on an `unordered_map` to work via const access (it does, `find` has a const overload).

NOTE: `m_flConnectionQualityRemote` is a value in [0, 1] where 1 = perfect. We convert to packet-loss percentage by `(1 - quality) * 100`. GNS docs note this is an estimate based on round-trip ACKs, not a direct loss measurement; close enough for a debug HUD.

- [ ] **Step 4: Build everything to confirm nothing broke**

```powershell
cmake --build build
```

Use `timeout: 300000`. Expected: clean build of `ironcore` + all consumers.

- [ ] **Step 5: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all tests pass (no test regressions; `stats()` isn't unit-tested).

- [ ] **Step 6: Commit**

```powershell
git add engine/net/NetworkStats.h engine/net/GnsTransport.h engine/net/GnsTransport.cpp
git commit -m "Engine: GnsTransport.stats(ConnectionId) returns ConnectionStats POD" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 5: HUD widget — `addNetworkStatsWidget` + `updateNetworkStats`

**Files:**
- Modify: `engine/ui/Hud.h`
- Modify: `engine/ui/Hud.cpp`

Convenience wrapper over the existing `addText`/`setText` API. Game wires
three lines per frame: construct widget once, update with a ConnectionStats
each frame.

- [ ] **Step 1: Add the handle struct + two method declarations to `engine/ui/Hud.h`**

Use the Edit tool. Add an include at the top of the file (after the existing includes):

```cpp
#include "net/NetworkStats.h"
```

Inside the `Hud` class (`public:` section), AFTER the existing `void setVisible(HudId id, bool visible);` line, add:

```cpp

    // ---------- network-stats convenience ----------
    // A NetStatsHudHandle bundles the HudIds for the 4 lines we render:
    // ping, packet loss, in/out bandwidth, connection state. The game
    // creates one with addNetworkStatsWidget and updates it each frame
    // with updateNetworkStats(handle, stats).
    struct NetStatsHudHandle {
        HudId pingId;
        HudId lossId;
        HudId bandwidthId;
        HudId stateId;
    };

    // Register the 4 lines anchored at `topRight` (text right-aligned
    // would be nice; for now we left-anchor at (topRight.x - widthGuess,
    // topRight.y) and let game tweak). Returns the handle.
    NetStatsHudHandle addNetworkStatsWidget(
        Vec2 topRight, Vec4 color = Vec4{1.0f, 1.0f, 1.0f, 0.7f});

    // Update the widget's 4 text lines from a fresh ConnectionStats.
    void updateNetworkStats(const NetStatsHudHandle& h,
                            const ConnectionStats& s);
```

- [ ] **Step 2: Implement the two methods in `engine/ui/Hud.cpp`**

Add at the bottom of the file, just before the closing namespace brace. (Confirm the existing namespace closing matches.)

```cpp

Hud::NetStatsHudHandle Hud::addNetworkStatsWidget(Vec2 topRight, Vec4 color) {
    // Left-anchor each line to the left of `topRight` with a rough fixed
    // pixel width. Caller can reposition individual ids if needed.
    constexpr float kWidthGuess = 220.0f;
    constexpr float kLineHeight = 22.0f;
    const Vec2 base{topRight.x - kWidthGuess, topRight.y};
    NetStatsHudHandle h;
    h.stateId     = addText("state: ?",     {base.x, base.y + 0 * kLineHeight}, 1.5f, color);
    h.pingId      = addText("ping: ? ms",   {base.x, base.y + 1 * kLineHeight}, 1.5f, color);
    h.lossId      = addText("loss: ? %",    {base.x, base.y + 2 * kLineHeight}, 1.5f, color);
    h.bandwidthId = addText("bw: ? in/? out kbps", {base.x, base.y + 3 * kLineHeight}, 1.5f, color);
    return h;
}

void Hud::updateNetworkStats(const NetStatsHudHandle& h,
                              const ConnectionStats& s) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "state: %s", s.state.c_str());
    setText(h.stateId, buf);
    std::snprintf(buf, sizeof(buf), "ping: %.0f ms", s.pingMs);
    setText(h.pingId, buf);
    std::snprintf(buf, sizeof(buf), "loss: %.1f %%", s.packetLossPct);
    setText(h.lossId, buf);
    std::snprintf(buf, sizeof(buf), "bw: %.1f in / %.1f out kbps",
                  s.bandwidthInKbps, s.bandwidthOutKbps);
    setText(h.bandwidthId, buf);
}
```

Add at the TOP of `engine/ui/Hud.cpp` (after existing includes) if not already present:
```cpp
#include <cstdio>
```

- [ ] **Step 3: Build everything**

```powershell
cmake --build build
```

Use `timeout: 180000`. Expected: clean build.

- [ ] **Step 4: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 5: Commit**

```powershell
git add engine/ui/Hud.h engine/ui/Hud.cpp
git commit -m "Engine: Hud.addNetworkStatsWidget for in-game net diagnostics" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 6: Refactor `games/05-net-cubes`

**Files:**
- Modify: `games/05-net-cubes/Messages.h`
- Modify: `games/05-net-cubes/main.cpp`

Three concerns in one task: add `kGameId` + reject mismatched ID; adopt
`TimeHistory<Vec3>` for remote-cube rendering; add the net-stats HUD.

- [ ] **Step 1: Update `games/05-net-cubes/Messages.h`**

Use Edit. Replace the entire current file with:

```cpp
#pragma once

#include <cstdint>

namespace iron::netcubes {

// 4-byte ASCII game identifier. Sent in every HelloMsg; client rejects
// a Hello whose gameId doesn't match. Prevents this game from being
// accidentally connected to a different iron-core network exe (e.g.
// net-tag, which uses a different value).
//   'n', 'E', 't', 'B' → 0x6E45'7442
constexpr std::uint32_t kGameId = 0x6E457442u;

struct HelloMsg {
    static constexpr std::uint8_t kTag = 1;
    std::uint32_t gameId;   // = kGameId on send; receiver rejects on mismatch
    std::uint32_t peerId;
};

struct PositionMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t peerId;
    float x, y, z;
};

}  // namespace iron::netcubes
```

- [ ] **Step 2: Update `games/05-net-cubes/main.cpp` — includes**

Use Edit. After the existing `#include "core/NetArgs.h"` line, add:

```cpp
#include "net/TimeHistory.h"
```

- [ ] **Step 3: Replace the `CubeState` map with `TimeHistory<Vec3>` map**

Use Edit. Find this block (around the existing comment "peer cube state"):

```cpp
    struct CubeState { iron::Vec3 displayed; iron::Vec3 target; };
    std::unordered_map<std::uint32_t, CubeState> cubes;
    constexpr float kCubeSmoothness = 12.0f;
```

Replace with:

```cpp
    // Remote peer positions are interpolated through a per-peer
    // TimeHistory<Vec3> for jitter/loss tolerance. Local player is NOT
    // buffered — the local cube renders at player.position directly.
    std::unordered_map<std::uint32_t, iron::TimeHistory<iron::Vec3>> remoteHistories;
    constexpr auto kDisplayDelay = std::chrono::milliseconds{100};
```

- [ ] **Step 4: Update the HelloMsg send (host) to include `gameId = kGameId`**

Use Edit. Find the `setOnConnectionOpened` lambda's `registry.send<HelloMsg>` call. The current call passes `HelloMsg{assigned}`. Change to `HelloMsg{iron::netcubes::kGameId, assigned}`.

- [ ] **Step 5: Update the HelloMsg handler (client) to reject mismatched gameId**

Use Edit. Find the `registry.registerHandler<iron::netcubes::HelloMsg>` lambda. The current body looks like:

```cpp
    registry.registerHandler<iron::netcubes::HelloMsg>(
        [&](iron::ConnectionId /*c*/, const iron::netcubes::HelloMsg& msg) {
            if (isHost) {
                iron::Log::warn("net-cubes: host received Hello — ignoring");
                return;
            }
            if (myPeerId == 0) {
                myPeerId = msg.peerId;
            }
        });
```

Replace with:

```cpp
    registry.registerHandler<iron::netcubes::HelloMsg>(
        [&](iron::ConnectionId /*c*/, const iron::netcubes::HelloMsg& msg) {
            if (isHost) {
                iron::Log::warn("net-cubes: host received Hello — ignoring");
                return;
            }
            if (msg.gameId != iron::netcubes::kGameId) {
                iron::Log::error(
                    "net-cubes: connected to wrong game (gameId=0x%08X, expected 0x%08X) — exiting",
                    msg.gameId, iron::netcubes::kGameId);
                glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
                return;
            }
            if (myPeerId == 0) {
                myPeerId = msg.peerId;
            }
        });
```

- [ ] **Step 6: Replace the PositionMsg handler's CubeState update with TimeHistory.push**

Use Edit. Find the existing `registerHandler<iron::netcubes::PositionMsg>` body, specifically the block that does:

```cpp
            const iron::Vec3 incoming{msg.x, msg.y, msg.z};
            auto [it, inserted] = cubes.try_emplace(msg.peerId);
            if (inserted) {
                it->second.displayed = incoming;
            }
            it->second.target = incoming;
```

Replace with:

```cpp
            const iron::Vec3 incoming{msg.x, msg.y, msg.z};
            remoteHistories[msg.peerId].push(incoming);
```

- [ ] **Step 7: Remove the per-frame cube lerp + per-frame `cubes[myId] = ...` block**

Use Edit. Find this block in the main loop (after `transport.poll()`):

```cpp
        const std::uint32_t myId = isHost ? 0u : myPeerId;
        const bool haveIdentity = isHost || (myPeerId != 0);
        if (haveIdentity) {
            cubes[myId] = CubeState{player.position, player.position};
        }

        // Lerp every remote cube's displayed position toward its latest
        // target. Framerate-independent: 1 - exp(-dt * smoothness).
        const float cubeLerp = 1.0f - std::exp(-dt * kCubeSmoothness);
        for (auto& [peerId, cube] : cubes) {
            if (peerId == myId) continue;
            cube.displayed.x += (cube.target.x - cube.displayed.x) * cubeLerp;
            cube.displayed.y += (cube.target.y - cube.displayed.y) * cubeLerp;
            cube.displayed.z += (cube.target.z - cube.displayed.z) * cubeLerp;
        }
```

Replace with:

```cpp
        const std::uint32_t myId = isHost ? 0u : myPeerId;
        const bool haveIdentity = isHost || (myPeerId != 0);
        // The local cube is rendered directly from player.position in
        // the render block below. Remote cubes come from their per-peer
        // TimeHistory<Vec3> sampled at (now - displayDelay).
```

- [ ] **Step 8: Replace the cube render loop to use TimeHistory**

Use Edit. Find the cube render loop (currently iterates `cubes`):

```cpp
        for (const auto& [peerId, cube] : cubes) {
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = litShader;
            call.model = iron::translation(cube.displayed);
            call.material.texture     = renderer.whiteTexture();
            call.material.normalMap   = renderer.flatNormalTexture();
            call.material.specularMap = renderer.noSpecularTexture();
            call.material.emissive    = colorForPeer(peerId) * 0.4f;
            renderer.submit(call);
        }
```

Replace with:

```cpp
        // Local cube renders at player.position (no buffering — your
        // own movement should be instant).
        if (haveIdentity) {
            iron::DrawCall localCall;
            localCall.mesh = cubeMesh;
            localCall.shader = litShader;
            localCall.model = iron::translation(player.position);
            localCall.material.texture     = renderer.whiteTexture();
            localCall.material.normalMap   = renderer.flatNormalTexture();
            localCall.material.specularMap = renderer.noSpecularTexture();
            localCall.material.emissive    = colorForPeer(myId) * 0.4f;
            renderer.submit(localCall);
        }
        // Remote cubes render at the buffer-interpolated position.
        for (const auto& [peerId, history] : remoteHistories) {
            if (peerId == myId) continue;  // skip our own echo if host
            auto pos = history.sampleAtDelay(kDisplayDelay);
            if (!pos) continue;
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = litShader;
            call.model = iron::translation(*pos);
            call.material.texture     = renderer.whiteTexture();
            call.material.normalMap   = renderer.flatNormalTexture();
            call.material.specularMap = renderer.noSpecularTexture();
            call.material.emissive    = colorForPeer(peerId) * 0.4f;
            renderer.submit(call);
        }
```

- [ ] **Step 9: Update the HUD `Peers:` count to use `remoteHistories.size() + 1` (or 0 if no identity)**

Use Edit. Find the `hud.setText(peersText, "Peers: " + std::to_string(cubes.size()));` line. Replace with:

```cpp
        const std::size_t peerCount = (haveIdentity ? 1u : 0u) + remoteHistories.size();
        hud.setText(peersText, "Peers: " + std::to_string(peerCount));
```

NOTE: this counts the LOCAL peer (if we have an identity) plus all remote
peers we've seen. Matches the previous semantic that `cubes.size()`
implied.

- [ ] **Step 10: Wire the net-stats HUD widget**

Use Edit. Find the HUD setup block (where `peersText` is added). After it, add:

```cpp
    auto netStatsHud = hud.addNetworkStatsWidget(iron::Vec2{1268.0f, 12.0f});
```

(`1268.0f` = `kScreenWidth(1280) - 12.0f` margin.)

In the per-frame HUD update block (where `hud.setText(roleText, ...)` runs), append after the existing HUD updates and BEFORE `renderer.drawHud(...)`:

```cpp
        // Update the live network stats widget for the connection that
        // matters: host shows the first connected client (if any),
        // client shows the host connection.
        iron::ConnectionId statsConn = iron::kInvalidConnection;
        if (isHost) {
            if (!connToPeerId.empty()) statsConn = connToPeerId.begin()->first;
        } else {
            statsConn = hostConn;
        }
        hud.updateNetworkStats(netStatsHud, transport.stats(statsConn));
```

- [ ] **Step 11: Drop any now-unused `<cmath>` exp/lerp helpers** — none to drop; the file already includes `<cmath>` for chase-cam math. Leave the include.

- [ ] **Step 12: Build**

```powershell
cmake --build build --target net-cubes
```

Use `timeout: 180000`. Expected: clean build.

- [ ] **Step 13: Smoke (skip if headless)**

Open two PowerShell windows:
```powershell
./build/games/05-net-cubes/Debug/net-cubes.exe
./build/games/05-net-cubes/Debug/net-cubes.exe --connect 127.0.0.1
```

Verify:
- HUD top-left shows role + peers (functionally unchanged).
- HUD top-right shows state/ping/loss/bandwidth lines (NEW).
- Cubes glide smoothly between updates instead of stepping (NEW).
- Launching `net-tag.exe` as the second instance should NOW print
  `[ERROR] net-cubes: connected to wrong game (gameId=...)` and exit (NEW).

- [ ] **Step 14: Commit**

```powershell
git add games/05-net-cubes/Messages.h games/05-net-cubes/main.cpp
git commit -m "Net-cubes: TimeHistory interpolation + game-id reject + net-stats HUD" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 7: Refactor `games/06-net-tag`

**Files:**
- Modify: `games/06-net-tag/Messages.h`
- Modify: `games/06-net-tag/main.cpp`

Same changes as cubes (gameId, TimeHistory, net-stats HUD), plus
adopt `FixedTickScheduler` for the score-broadcast cadence.

- [ ] **Step 1: Update `games/06-net-tag/Messages.h`**

Use Write to replace the entire file (it has 6 message types; keep them
all, just add `kGameId` constant + extend `HelloMsg`):

```cpp
#pragma once

#include <cstdint>

namespace iron::nettag {

// 4-byte ASCII game identifier. 't', 'A', 'G', 'o' → 0x7441'476F.
// Sent in every HelloMsg; client rejects a Hello whose gameId doesn't
// match. Prevents this game from being accidentally connected to a
// different iron-core network exe (e.g. net-cubes).
constexpr std::uint32_t kGameId = 0x7441476Fu;

struct HelloMsg {
    static constexpr std::uint8_t kTag = 1;
    std::uint32_t gameId;   // = kGameId on send; receiver rejects on mismatch
    std::uint32_t peerId;
};

struct PositionMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t peerId;
    float x, y, z;
};

struct TagSwapMsg {
    static constexpr std::uint8_t kTag = 3;
    std::uint32_t newItPeerId;
};

struct ScoreUpdateMsg {
    static constexpr std::uint8_t kTag = 4;
    std::uint32_t peerId;
    float itTimeSec;
};

struct RoundStartMsg {
    static constexpr std::uint8_t kTag = 5;
    std::uint32_t initialItPeerId;
    float roundDurationSec;
};

struct RoundEndMsg {
    static constexpr std::uint8_t kTag = 6;
    std::uint32_t winnerPeerId;
};

}  // namespace iron::nettag
```

- [ ] **Step 2: Update `games/06-net-tag/main.cpp` — includes**

Add at the top of the file (after the existing `#include "Messages.h"` block):

```cpp
#include "core/FixedTickScheduler.h"
#include "net/TimeHistory.h"
```

- [ ] **Step 3: Replace the `CubeState` map with `TimeHistory<Vec3>` map**

Use Edit. Find this block:

```cpp
    struct CubeState { iron::Vec3 displayed; iron::Vec3 target; };
    std::unordered_map<std::uint32_t, CubeState> cubes;
    constexpr float kCubeSmoothness = 12.0f;
```

Replace with:

```cpp
    // Remote peer positions are interpolated through a per-peer
    // TimeHistory<Vec3> for jitter/loss tolerance. Local player is NOT
    // buffered — local cube renders at player.position directly.
    std::unordered_map<std::uint32_t, iron::TimeHistory<iron::Vec3>> remoteHistories;
    constexpr auto kDisplayDelay = std::chrono::milliseconds{100};
```

Note this map needs to be referenced wherever the old `cubes` was. The
existing tag gameplay tick reads remote positions via `cubes.count(peerId)`
and `cubes[peerId].target` for distance checks (collision detection on
host). Those need to read from `remoteHistories[peerId].sample(now)`
instead — but for HOST gameplay, we want the LATEST known position
(not delayed). Continue to step 4 for that detail.

- [ ] **Step 4: Add a helper for "latest known position of a peer" (host gameplay)**

Use Edit. Find where the `cubes` map is referenced inside the host tag-detection block — the lines `cubes.count(itPeerId)` / `cubes[itPeerId].target` and analogous for the candidate peer. The new lookup needs to use the LATEST sample in the TimeHistory, not the display-delayed one (the host runs authoritative tag detection against current state).

Replace the helper `(pid == 0) ? player.position : (cubes.count(pid) ? cubes[pid].target : iron::Vec3{1e9f, 0, 0})` with a local lambda placed BEFORE the host gameplay tick block:

```cpp
        // Host-side helper: latest known position of any peer.
        //   - peerId 0 → local host player.position (instant).
        //   - other peers → most recent TimeHistory sample (NOT the
        //     display-delayed one — gameplay uses live data).
        auto latestPosition = [&](std::uint32_t pid) -> iron::Vec3 {
            if (pid == 0) return player.position;
            auto it = remoteHistories.find(pid);
            if (it == remoteHistories.end()) return iron::Vec3{1e9f, 0, 0};
            auto s = it->second.sample(
                iron::TimeHistory<iron::Vec3>::Clock::now());
            return s.value_or(iron::Vec3{1e9f, 0, 0});
        };
```

Then in the host tag-detection block, replace the existing pos-lookup expressions:

```cpp
                const iron::Vec3& itPos = (itPeerId == 0)
                    ? player.position
                    : (cubes.count(itPeerId) ? cubes[itPeerId].target : iron::Vec3{1e9f, 0, 0});
```

with:

```cpp
                const iron::Vec3 itPos = latestPosition(itPeerId);
```

And:

```cpp
                    const iron::Vec3& pos = (pid == 0)
                        ? player.position
                        : (cubes.count(pid) ? cubes[pid].target : iron::Vec3{1e9f, 0, 0});
```

with:

```cpp
                    const iron::Vec3 pos = latestPosition(pid);
```

- [ ] **Step 5: Update the HelloMsg send (host) to include `gameId = kGameId`**

Same change as cubes Task 6 Step 4. Find the host's `registry.send<HelloMsg>` calls (there are TWO in net-tag after the late-joiner fix from M8.3: one in `onConnectionOpened`, one in the same lambda's late-joiner branch sending the snapshot is `RoundStartMsg` not HelloMsg — only the one HelloMsg send to update).

Change `iron::nettag::HelloMsg{assigned}` to `iron::nettag::HelloMsg{iron::nettag::kGameId, assigned}`.

- [ ] **Step 6: Update the HelloMsg handler to reject mismatched gameId**

Same change as cubes Task 6 Step 5, but for `iron::nettag::HelloMsg` and using `iron::nettag::kGameId`:

```cpp
    registry.registerHandler<iron::nettag::HelloMsg>(
        [&](iron::ConnectionId /*c*/, const iron::nettag::HelloMsg& msg) {
            if (isHost) {
                iron::Log::warn("net-tag: host received Hello — ignoring");
                return;
            }
            if (msg.gameId != iron::nettag::kGameId) {
                iron::Log::error(
                    "net-tag: connected to wrong game (gameId=0x%08X, expected 0x%08X) — exiting",
                    msg.gameId, iron::nettag::kGameId);
                glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
                return;
            }
            if (myPeerId == 0) myPeerId = msg.peerId;
        });
```

- [ ] **Step 7: Replace the PositionMsg handler's CubeState update with TimeHistory.push**

Find the existing PositionMsg handler block:

```cpp
            const iron::Vec3 incoming{msg.x, msg.y, msg.z};
            auto [it, inserted] = cubes.try_emplace(msg.peerId);
            if (inserted) it->second.displayed = incoming;
            it->second.target = incoming;
```

Replace with:

```cpp
            const iron::Vec3 incoming{msg.x, msg.y, msg.z};
            remoteHistories[msg.peerId].push(incoming);
```

- [ ] **Step 8: Remove the per-frame remote-cube lerp**

Find this block in the main loop (after `transport.poll()`):

```cpp
        // Lerp remote cubes toward their latest target.
        const float cubeLerp = 1.0f - std::exp(-dt * kCubeSmoothness);
        for (auto& [peerId, cube] : cubes) {
            if (peerId == myId) continue;
            cube.displayed.x += (cube.target.x - cube.displayed.x) * cubeLerp;
            cube.displayed.y += (cube.target.y - cube.displayed.y) * cubeLerp;
            cube.displayed.z += (cube.target.z - cube.displayed.z) * cubeLerp;
        }
```

DELETE the entire block. The TimeHistory handles smoothing in `sampleAtDelay` at render time.

- [ ] **Step 9: Remove the per-frame `cubes[myId] = ...` assignment**

Find:

```cpp
        if (haveIdentity) {
            cubes[myId] = CubeState{player.position, player.position};
        }
```

DELETE it. The local cube renders from `player.position` directly in the render block.

- [ ] **Step 10: Update the cube render loop to use TimeHistory + draw local**

Find the cube render loop and replace it the same way as cubes Task 6 Step 8 (but with the cleanup detail that net-tag has the "it" red glow). Concretely:

```cpp
        // Local cube renders at player.position (no buffering).
        if (haveIdentity) {
            iron::DrawCall localCall;
            localCall.mesh = cubeMesh;
            localCall.shader = litShader;
            localCall.model = iron::translation(player.position);
            localCall.material.texture     = renderer.whiteTexture();
            localCall.material.normalMap   = renderer.flatNormalTexture();
            localCall.material.specularMap = renderer.noSpecularTexture();
            iron::Vec3 emissive = colorForPeer(myId) * 0.4f;
            if (myId == itPeerId) emissive = emissive + iron::Vec3{1.5f, 0.0f, 0.0f};
            localCall.material.emissive = emissive;
            renderer.submit(localCall);
        }
        // Remote cubes at buffer-interpolated positions.
        for (const auto& [peerId, history] : remoteHistories) {
            if (peerId == myId) continue;
            auto pos = history.sampleAtDelay(kDisplayDelay);
            if (!pos) continue;
            iron::DrawCall call;
            call.mesh = cubeMesh;
            call.shader = litShader;
            call.model = iron::translation(*pos);
            call.material.texture     = renderer.whiteTexture();
            call.material.normalMap   = renderer.flatNormalTexture();
            call.material.specularMap = renderer.noSpecularTexture();
            iron::Vec3 emissive = colorForPeer(peerId) * 0.4f;
            if (peerId == itPeerId) emissive = emissive + iron::Vec3{1.5f, 0.0f, 0.0f};
            call.material.emissive = emissive;
            renderer.submit(call);
        }
```

- [ ] **Step 11: Update the HUD `Peers:` count similarly**

Find the line that sets `peersText` in the HUD update block. Replace `std::to_string(cubes.size())` with:

```cpp
        const std::size_t peerCount = (haveIdentity ? 1u : 0u) + remoteHistories.size();
        hud.setText(peersText, "Peers: " + std::to_string(peerCount));
```

Also: the scoreboard build code reads `cubes` indirectly? It doesn't —
it iterates `players` (host) or `clientScores` (client). Leave that
alone.

- [ ] **Step 12: Adopt `FixedTickScheduler` for the score-broadcast cadence**

Find the gameplay tick block on host where it computes
`gameElapsedSec - lastScoreBroadcastSec >= kScoreBroadcastIntervalSec`
and broadcasts scores. Replace with a `FixedTickScheduler` instance
that fires once per second.

ADD this declaration alongside the existing gameplay-state declarations
(near `gameElapsedSec`, `lastScoreBroadcastSec`):

```cpp
    iron::FixedTickScheduler scoreTicker{std::chrono::seconds{1}};
```

REPLACE the existing block:

```cpp
                // Broadcast scores every 1 s.
                if (gameElapsedSec - lastScoreBroadcastSec >= kScoreBroadcastIntervalSec) {
                    lastScoreBroadcastSec = gameElapsedSec;
                    for (const auto& [pid, info] : players) {
                        const iron::nettag::ScoreUpdateMsg sMsg{pid, info.itTimeAccumSec};
                        for (const auto& [c, _] : connToPeerId) {
                            registry.send<iron::nettag::ScoreUpdateMsg>(
                                c, sMsg, iron::SendReliability::Reliable);
                        }
                    }
                }
```

WITH:

```cpp
                // Broadcast scores at the FixedTickScheduler cadence
                // (1 Hz). The scheduler may fire 0 or 1 times per frame
                // depending on accumulated dt.
                scoreTicker.update(dt, [&]() {
                    for (const auto& [pid, info] : players) {
                        const iron::nettag::ScoreUpdateMsg sMsg{pid, info.itTimeAccumSec};
                        for (const auto& [c, _] : connToPeerId) {
                            registry.send<iron::nettag::ScoreUpdateMsg>(
                                c, sMsg, iron::SendReliability::Reliable);
                        }
                    }
                });
```

The local `lastScoreBroadcastSec` and `kScoreBroadcastIntervalSec`
become unused. DELETE them from their declarations.

- [ ] **Step 13: Wire the net-stats HUD widget**

Same change as cubes Task 6 Step 10. Add to the HUD setup after
`scoresText` is added:

```cpp
    auto netStatsHud = hud.addNetworkStatsWidget(iron::Vec2{1268.0f, 12.0f});
```

And in the per-frame HUD update block, append before `drawHud(...)`:

```cpp
        iron::ConnectionId statsConn = iron::kInvalidConnection;
        if (isHost) {
            if (!connToPeerId.empty()) statsConn = connToPeerId.begin()->first;
        } else {
            statsConn = hostConn;
        }
        hud.updateNetworkStats(netStatsHud, transport.stats(statsConn));
```

- [ ] **Step 14: Build**

```powershell
cmake --build build --target net-tag
```

Use `timeout: 180000`. Expected: clean build.

- [ ] **Step 15: Run all tests**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 16: Smoke (skip if headless)**

Launch two instances. Verify:
- Same gameplay as M8.3 (round timer, tag swap with cooldown, scoring,
  winner, late-joiner snapshot).
- Net-stats HUD lines visible top-right.
- Cubes glide smoothly between 30Hz updates (NEW).
- Launching `net-cubes.exe` as the second instance produces
  `[ERROR] net-tag: connected to wrong game (gameId=...)` and exits
  (NEW).

- [ ] **Step 17: Commit**

```powershell
git add games/06-net-tag/Messages.h games/06-net-tag/main.cpp
git commit -m "Net-tag: TimeHistory + game-id + net-stats HUD + FixedTickScheduler" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 8: Docs

**Files:**
- Modify: `docs/engine/networking.md`

Append three new sections covering TimeHistory, game-id handshake, and
the debug HUD.

- [ ] **Step 1: Append to `docs/engine/networking.md`**

Use the Edit tool. Find the last paragraph of the current file
(should end with text about net-tag) and append:

```markdown

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
detection: host maintains a history of every player's authoritative
position; on `FireMsg{origin, dir, fireTimestamp}`, the server queries
`history.sample(fireTimestamp)` for each player and validates hits
against where they actually were when the client fired. That's a
future-milestone game-side use; the primitive is here today.

## Game-id handshake

Every game advertises a 4-byte ASCII identifier in its `HelloMsg`. The
client rejects a Hello whose `gameId` doesn't match. This prevents
accidentally connecting two different iron-core network exes to each
other (the cubes/tag interop bug we hit in M8.3 playtest).

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
honored on average regardless of framerate jitter.

## Network debug HUD

Both networked demos render a small "network stats" widget in the
top-right corner with the live ping, packet loss, in/out bandwidth, and
connection state for the current connection. Powered by:

```cpp
ConnectionStats GnsTransport::stats(ConnectionId conn) const;
```

which pulls from `ISteamNetworkingSockets::GetConnectionRealTimeStatus`.
The `iron::Hud::addNetworkStatsWidget(...)` / `updateNetworkStats(...)`
pair wraps this into 3 lines of game code:

```cpp
auto netStatsHud = hud.addNetworkStatsWidget({1268, 12});
// each frame:
hud.updateNetworkStats(netStatsHud, transport.stats(activeConn));
```

Useful when diagnosing "is this peer actually connected?", "why is the
sync jittery?", or "is bandwidth blowing up?".
```

- [ ] **Step 2: Commit**

```powershell
git add docs/engine/networking.md
git commit -m "Docs: TimeHistory + game-id + FixedTickScheduler + net-stats HUD (M8.4)" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 9: Code review + PR

**Files:** none modified — read-only review (plus any fixes the review surfaces)

- [ ] **Step 1: Show the diff range**

```powershell
git log --oneline main..HEAD
git diff main --stat
```

Expected: 8 commits (Tasks 1–8).

- [ ] **Step 2: Build + full ctest + smoke-run both refactored exes**

```powershell
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
./build/games/05-net-cubes/Debug/net-cubes.exe  # smoke (close after a second)
./build/games/06-net-tag/Debug/net-tag.exe      # smoke (close after a second)
```

Use `timeout: 300000` for the build. Expected: clean build, all tests pass, both exes launch.

- [ ] **Step 3: Dispatch code-quality reviewer**

Dispatch `feature-dev:code-reviewer` (or `general-purpose`) with:

> Review the M8.4 net-foundation-polish changes (`git diff main`) in the Iron Core Engine. Milestone adds 4 engine primitives (`iron::TimeHistory<T>`, `iron::FixedTickScheduler`, `iron::ConnectionStats` + `GnsTransport::stats()`, `iron::Hud::addNetworkStatsWidget`) plus refactors net-cubes and net-tag to use them, adds a 4-byte game-id handshake to both games, and uses FixedTickScheduler for net-tag's score broadcast. Spec: `docs/superpowers/specs/2026-05-24-net-foundation-polish-design.md`.
>
> Focus on:
> 1. **TimeHistory correctness** — empty/single/middle/before/after/eviction edge cases; query interpolation math; thread-safety isn't claimed but read-after-push consistency.
> 2. **FixedTickScheduler determinism** — accumulator preserves remainder across calls; catches up on long frames; correct semantics for short frames; no float drift issues.
> 3. **GnsTransport::stats** — correctly maps GNS RealTimeStatus to ConnectionStats; const-correctness; safe on invalid connection id.
> 4. **Game-id handshake** — host always sets gameId; client always validates BEFORE acting on the message; mismatched ID closes the window cleanly; cross-game connect produces a clear error.
> 5. **Refactor parity** — cubes & tag observable behavior unchanged (round timer, tag swap, scoring, peer count, ESC, chase camera) PLUS new smoothness on remote cubes.
> 6. **HUD widget** — handle struct is opaque to game; updateNetworkStats handles all 4 lines; no crash on default-constructed stats.
> 7. **Header hygiene** — no `<steam/...>` leaked into games or non-net engine code.
>
> Skip style nits. Cap at 10 findings. Under 400 words. End with **APPROVED** or **NEEDS_FIXES**.

- [ ] **Step 4: Address findings**

Apply fixes. Push back on cosmetic suggestions; only block on real correctness issues.

- [ ] **Step 5: Final verification**

```powershell
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

Expected: clean build, all tests pass.

- [ ] **Step 6: Commit review fixes (skip if none)**

```powershell
git add -A
git commit -m "M8.4: address code-review findings" -m "" -m "Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

- [ ] **Step 7: Push and open the PR**

```powershell
git push -u origin feat/net-foundation-polish
gh pr create --title "Milestone 8.4: Networking foundation polish (TimeHistory + game-id + tick scheduler + debug HUD)" --body "$(cat <<'EOF'
## Summary

Four engine primitives that unblock M8.5 (prediction + helpers) and M8.6 (hero shooter):

- **`iron::TimeHistory<T>`** (header-only template) — time-stamped sample buffer with interpolation query. Used client-side for smooth remote-player display (~100 ms display delay, jitter/loss tolerant). Same primitive will be used server-side in M8.6 for lag-compensated hit detection.
- **`iron::FixedTickScheduler`** (header-only) — accumulator-pattern fixed-timestep helper. `update(dt, tickFn)` invokes `tickFn` deterministically once per tick interval regardless of render framerate. Required for prediction (M8.5).
- **`iron::ConnectionStats`** struct + `GnsTransport::stats(ConnectionId)` const accessor — pulls live ping / loss / bandwidth / state from GNS.
- **`iron::Hud::addNetworkStatsWidget`** + `updateNetworkStats` — 3-line convenience to put a live network-health widget in any game.

Both networked demos adopt all four:

- net-cubes & net-tag both gain TimeHistory-driven smooth remote rendering, a 4-byte `gameId` handshake (prevents the cross-connect footgun we hit), and the top-right net-stats HUD.
- net-tag additionally uses `FixedTickScheduler` for the 1 Hz score broadcast.

## Test plan

- [x] `test_time_history` passes (6 cases: empty/single/middle/before/after/eviction)
- [x] `test_fixed_tick_scheduler` passes (4 cases: catch-up, short-frame, remainder, exact)
- [x] All existing tests still pass
- [x] Both games build + launch
- [x] Manual: cubes and tag remote-player movement is visibly smoother
- [x] Manual: cross-connect (net-cubes ↔ net-tag) now produces a clean error + exit on the client

## Out of scope (M8.5+)

- `iron::PeerManager` (extract repeated peerId/conn/Hello flow) — M8.5
- `iron::PredictionEngine<TInput, TState>` — M8.5
- Server-authoritative refactor of net-tag — M8.5
- Snapshot pattern docs + reference impl — M8.5
- Lag-compensation pattern (uses TimeHistory server-side) — M8.6 alongside hero shooter
- Hero shooter game itself — M8.6

EOF
)"
```

Return the PR URL.

---

## Self-review (run after writing the plan, before handoff)

Already done inline:

- **Spec coverage:**
  - `iron::TimeHistory<T>` + 6 test cases + sampleAtDelay convenience → Task 2
  - `iron::FixedTickScheduler` + 4 test cases → Task 3
  - `iron::ConnectionStats` + `GnsTransport::stats()` → Task 4
  - `iron::Hud::addNetworkStatsWidget` + `updateNetworkStats` → Task 5
  - `iron::interpolate(Vec3, Vec3, float)` (required by TimeHistory via ADL) → Task 1
  - Game-id handshake (`kGameId` const + `HelloMsg.gameId` + reject on mismatch) → Task 6 (cubes) and Task 7 (tag)
  - TimeHistory adopted in both games → Task 6 / Task 7
  - FixedTickScheduler adopted in net-tag for score broadcast → Task 7
  - Net-stats HUD widget wired in both games → Task 6 / Task 7
  - Docs sections (TimeHistory, game-id, FixedTickScheduler, debug HUD) → Task 8
  - Code review → Task 9
- **Non-goals respected:** no prediction, no PeerManager extraction, no server-authoritative refactor, no snapshot abstraction, no lag-comp game-side impl, no hero shooter, no Strandbound integration.
- **Placeholder scan:** no TBD/TODO. Game-id values are explicit constants (`0x6E457442` for cubes, `0x7441476F` for tag).
- **Type consistency:**
  - `iron::TimeHistory<T>::Clock` / `TimePoint` / `sample` / `sampleAtDelay` / `push` / `size` / `clear` used consistently across header, tests, and both game refactors.
  - `iron::FixedTickScheduler::update(dt, tickFn)` signature consistent across header, test, and net-tag adoption.
  - `iron::ConnectionStats` field names (`pingMs`, `packetLossPct`, `jitterMs`, `bandwidthInKbps`, `bandwidthOutKbps`, `state`) consistent across struct definition, `GnsTransport::stats()` implementation, and HUD `updateNetworkStats` consumer.
  - `iron::Hud::NetStatsHudHandle` fields (`pingId`, `lossId`, `bandwidthId`, `stateId`) consistent across struct definition, `addNetworkStatsWidget` impl, and `updateNetworkStats` impl.
  - `iron::netcubes::kGameId` / `iron::nettag::kGameId` constants used consistently in Messages.h, HelloMsg send, and Hello receive validation.
