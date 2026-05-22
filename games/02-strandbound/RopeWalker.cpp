#include "RopeWalker.h"

#include <cstddef>

namespace {
// Lean dynamics.
constexpr float kInstability = 1.6f;  // lean self-amplification, per second
constexpr float kSteerRate = 1.9f;    // counter-steer authority, per second
// leanDriftMagnitude shaping.
constexpr float kDriftBase = 0.15f;    // perturbation/sec at mount
constexpr float kDriftGrowth = 0.05f;  // extra perturbation/sec per second
constexpr float kDriftMax = 0.6f;
}  // namespace

bool hasFooting(float x, float z, const std::vector<iron::Aabb>& islands) {
    for (const iron::Aabb& box : islands) {
        if (x >= box.min.x && x <= box.max.x &&
            z >= box.min.z && z <= box.max.z) {
            return true;
        }
    }
    return false;
}

float leanDriftMagnitude(float timeOnRope) {
    const float m = kDriftBase + kDriftGrowth * timeOnRope;
    return (m < kDriftMax) ? m : kDriftMax;
}

float applyLean(float lean, float nudge, float steer, float dt) {
    return lean + lean * kInstability * dt + nudge + steer * kSteerRate * dt;
}

float advanceParam(float t, float input, float walkSpeed, float ropeLength,
                   float dt) {
    if (ropeLength <= 0.0f) {
        return t;
    }
    float next = t + input * walkSpeed * dt / ropeLength;
    if (next < 0.0f) next = 0.0f;
    if (next > 1.0f) next = 1.0f;
    return next;
}

int findMountRope(iron::Vec3 playerFeet, const std::vector<iron::Rope>& ropes,
                  float radius, bool& outAtStart) {
    const float r2 = radius * radius;
    for (std::size_t i = 0; i < ropes.size(); ++i) {
        const std::vector<iron::VerletPoint>& pts = ropes[i].points();
        if (pts.size() < 2) {
            continue;
        }
        const iron::Vec3 a = pts.front().position;
        const iron::Vec3 b = pts.back().position;
        const float ax = a.x - playerFeet.x;
        const float az = a.z - playerFeet.z;
        if (ax * ax + az * az <= r2) {
            outAtStart = true;
            return static_cast<int>(i);
        }
        const float bx = b.x - playerFeet.x;
        const float bz = b.z - playerFeet.z;
        if (bx * bx + bz * bz <= r2) {
            outAtStart = false;
            return static_cast<int>(i);
        }
    }
    return -1;
}
