#pragma once

#include "math/Aabb.h"
#include "math/Vec.h"
#include "net/TimeHistory.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>

namespace iron {

// Server-side helper: records every player's authoritative position
// each tick, then rewinds to a client's view-time for hit validation.
// Internally backed by a TimeHistory<Vec3> per peer. The public API
// works in `double` seconds; the underlying chrono time_point uses a
// fixed epoch so the mapping is bijective for any given double.
class LagCompensator {
public:
    struct HitscanResult {
        std::uint32_t peerId;
        float distance;
        Vec3 point;
    };

    void recordPosition(std::uint32_t peerId, double timeSec, const Vec3& pos);
    void forgetPeer(std::uint32_t peerId);

    std::optional<Vec3> positionAt(
        std::uint32_t peerId, double rewindTimeSec) const;

    std::optional<Aabb> aabbAt(
        std::uint32_t peerId, double rewindTimeSec,
        const Vec3& halfExtents) const;

    // Cast a ray and return the closest lag-compensated player hit
    // (excluding `shooterPeerId`). Walls are NOT considered here; the
    // caller intersects the ray against world boxes separately and
    // picks whichever is closer.
    std::optional<HitscanResult> hitscan(
        std::uint32_t shooterPeerId,
        const Vec3& origin, const Vec3& dir,
        double rewindTimeSec,
        const Vec3& playerHalfExtents,
        std::span<const std::uint32_t> candidates) const;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static TimePoint toTp(double secs);

    std::unordered_map<std::uint32_t, TimeHistory<Vec3>> perPeer_;
};

}  // namespace iron
