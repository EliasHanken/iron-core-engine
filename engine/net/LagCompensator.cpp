#include "net/LagCompensator.h"

#include "math/Ray.h"

#include <chrono>

namespace iron {

LagCompensator::TimePoint LagCompensator::toTp(double secs) {
    return TimePoint{} + std::chrono::duration_cast<Clock::duration>(
                            std::chrono::duration<double>(secs));
}

void LagCompensator::recordPosition(std::uint32_t peerId, double timeSec,
                                    const Vec3& pos) {
    // Default-constructs a TimeHistory on first sample for this peer.
    perPeer_[peerId].push(toTp(timeSec), pos);
}

void LagCompensator::forgetPeer(std::uint32_t peerId) {
    perPeer_.erase(peerId);
}

std::optional<Vec3> LagCompensator::positionAt(
    std::uint32_t peerId, double rewindTimeSec) const {
    auto it = perPeer_.find(peerId);
    if (it == perPeer_.end()) return std::nullopt;
    return it->second.sample(toTp(rewindTimeSec));
}

std::optional<Aabb> LagCompensator::aabbAt(
    std::uint32_t peerId, double rewindTimeSec,
    const Vec3& halfExtents) const {
    const auto pos = positionAt(peerId, rewindTimeSec);
    if (!pos) return std::nullopt;
    return Aabb{
        Vec3{pos->x - halfExtents.x, pos->y - halfExtents.y, pos->z - halfExtents.z},
        Vec3{pos->x + halfExtents.x, pos->y + halfExtents.y, pos->z + halfExtents.z}};
}

std::optional<LagCompensator::HitscanResult> LagCompensator::hitscan(
    std::uint32_t shooterPeerId,
    const Vec3& origin, const Vec3& dir,
    double rewindTimeSec,
    const Vec3& playerHalfExtents,
    std::span<const std::uint32_t> candidates) const {
    std::optional<HitscanResult> best;
    const Ray ray{origin, dir};
    for (const auto pid : candidates) {
        if (pid == shooterPeerId) continue;
        const auto aabb = aabbAt(pid, rewindTimeSec, playerHalfExtents);
        if (!aabb) continue;
        float t = 0.0f;
        Vec3  n{};
        if (!intersectRayAabb(ray, *aabb, t, n)) continue;
        if (t < 0.0f) continue;
        if (!best || t < best->distance) {
            best = HitscanResult{
                pid, t,
                Vec3{origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t}};
        }
    }
    return best;
}

}  // namespace iron
