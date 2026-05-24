#pragma once

#include "math/Vec.h"  // brings iron::interpolate(Vec3, Vec3, float)

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace iron {

// Time-stamped sample buffer with interpolation query. Two use cases:
//   Client-side: sample(now - displayDelay) → smooth remote-peer
//   display, tolerant of jitter and modest packet loss.
//   Server-side: sample(client.fireTimestamp) → historical position
//   for hit-validation (lag compensation).
//
// T must support free-function `interpolate(T a, T b, float t)` (ADL).
// For iron::Vec3 the engine provides one in math/Vec.h.
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
    void push(const T& sample) { push(Clock::now(), sample); }

    // Push with explicit timestamp (test-friendly).
    void push(TimePoint at, const T& sample) {
        samples_.emplace_back(at, sample);
        // Evict samples older than (newest - retention). Keep one sample
        // before the window boundary as an anchor for interpolation —
        // i.e. only remove the front when the *second* entry is also
        // older than the cutoff. Using "newest" (not Clock::now()) keeps
        // the explicit-time API deterministic.
        const TimePoint cutoff = at - retention_;
        while (samples_.size() > 1 && samples_[1].first < cutoff) {
            samples_.pop_front();
        }
    }

    // Sample at absolute time. Clamps to earliest/latest if `at` is
    // outside the stored range. Linearly interpolates if straddling
    // two samples. Returns nullopt if buffer is empty.
    std::optional<T> sample(TimePoint at) const {
        if (samples_.empty()) return std::nullopt;
        if (at <= samples_.front().first) return samples_.front().second;
        if (at >= samples_.back().first)  return samples_.back().second;
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
