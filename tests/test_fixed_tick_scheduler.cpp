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

    // 30 Hz called 4 times with 0.01s each (40ms total). 40/33.333 = 1 tick.
    {
        FixedTickScheduler s{std::chrono::microseconds{33333}};
        int count = 0;
        for (int i = 0; i < 4; ++i) s.update(0.01f, [&]() { ++count; });
        CHECK(count == 1);
    }

    // 30 Hz, dt=2.0s → 60 invocations (catches up).
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
        CHECK(count == 0);  // 60ms < 100ms
        s.update(0.060f, [&]() { ++count; });
        CHECK(count == 1);  // 120ms ≥ 100ms; 20ms left
        s.update(0.080f, [&]() { ++count; });
        CHECK(count == 2);  // 20+80=100ms; another tick
    }

    return iron_test_result();
}
