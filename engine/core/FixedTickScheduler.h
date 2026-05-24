#pragma once

#include <chrono>
#include <utility>

namespace iron {

// Accumulator-pattern fixed-timestep scheduler. Game owns one and
// calls update(dtSeconds, tickFn) per frame. Invokes tickFn once per
// accumulated tickInterval — guarantees deterministic tick spacing
// independent of render framerate.
//
// Typical use:
//   iron::FixedTickScheduler ticker{std::chrono::milliseconds{33}};  // ~30 Hz
//   while (running) {
//       const float dt = frameDt();
//       ticker.update(dt, [&]() { simulationTick(); });
//       render();
//   }
//
// On a long frame, update fires tickFn multiple times to catch up.
// On a short frame, may fire zero times.
class FixedTickScheduler {
public:
    explicit FixedTickScheduler(std::chrono::microseconds tickInterval)
        : tickIntervalUs_(tickInterval.count()) {}

    template <typename TickFn>
    void update(float dtSeconds, TickFn&& tickFn) {
        // Convert dt to whole microseconds (round-nearest) and accumulate
        // in integer arithmetic to avoid floating-point drift over many frames.
        const auto dtUs = static_cast<long long>(dtSeconds * 1'000'000.0f + 0.5f);
        accumulatorUs_ += dtUs;
        while (accumulatorUs_ >= tickIntervalUs_) {
            accumulatorUs_ -= tickIntervalUs_;
            tickFn();
        }
    }

    // Tick interval in seconds (useful when game logic needs dt-per-tick
    // for physics integration).
    float tickIntervalSeconds() const {
        return static_cast<float>(tickIntervalUs_) * 1e-6f;
    }

private:
    long long tickIntervalUs_;
    long long accumulatorUs_ = 0;
};

}  // namespace iron
