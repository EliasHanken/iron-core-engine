#include "RopeWalker.h"

#include "math/Transform.h"

#include <cmath>
#include <cstddef>

namespace {
// Lean dynamics.
constexpr float kInstability = 1.6f;  // lean self-amplification, per second
constexpr float kSteerRate = 1.9f;    // counter-steer authority, per second
// leanDriftMagnitude shaping.
constexpr float kDriftBase = 0.15f;    // perturbation/sec at mount
constexpr float kDriftGrowth = 0.05f;  // extra perturbation/sec per second
constexpr float kDriftMax = 0.6f;
// Traversal tuning.
constexpr float kWalkSpeed = 3.0f;      // units/second along the rope
constexpr float kEyeHeight = 1.7f;      // camera height above the rope
constexpr float kMaxRoll = 0.5f;        // camera roll (radians) at |lean| = 1
constexpr float kMouseSensitivity = 0.0025f;
constexpr float kPitchLimit = 1.55334f;  // just under 90 degrees
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

// ---------------------------------------------------------------------------
// RopeWalker member functions
// ---------------------------------------------------------------------------

void RopeWalker::begin(const iron::Rope& rope, bool atStart, float yaw,
                       float pitch) {
    t_ = 0.0f;
    atStart_ = atStart;
    lean_ = 0.0f;
    timeOnRope_ = 0.0f;
    yaw_ = yaw;
    pitch_ = pitch;

    // Rope length is the sum of its segment lengths.
    const std::vector<iron::VerletPoint>& pts = rope.points();
    float len = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        len += iron::length(pts[i].position - pts[i - 1].position);
    }
    ropeLength_ = (len > 1e-4f) ? len : 1.0f;

    eye_ = sampleRope(rope) + iron::Vec3{0.0f, kEyeHeight, 0.0f};
    const iron::Vec3 m = mountEndPoint(rope);
    exitFeet_ = iron::Vec3{m.x, 0.0f, m.z};
}

RopeWalker::Result RopeWalker::step(float forward, float steer, float mouseDX,
                                    float mouseDY, float driftRandom, float dt,
                                    const iron::Rope& rope,
                                    const std::vector<iron::Aabb>& farIsland) {
    timeOnRope_ += dt;

    // Look — same convention as FirstPersonController.
    yaw_ -= mouseDX * kMouseSensitivity;
    pitch_ -= mouseDY * kMouseSensitivity;
    if (pitch_ > kPitchLimit) pitch_ = kPitchLimit;
    if (pitch_ < -kPitchLimit) pitch_ = -kPitchLimit;

    // Lean — an unstable balance the player must counter-steer.
    const float nudge = driftRandom * leanDriftMagnitude(timeOnRope_) * dt;
    lean_ = applyLean(lean_, nudge, steer, dt);
    if (lean_ >= 1.0f || lean_ <= -1.0f) {
        return Result::Fell;
    }

    // Move along the rope.
    t_ = advanceParam(t_, forward, kWalkSpeed, ropeLength_, dt);
    eye_ = sampleRope(rope) + iron::Vec3{0.0f, kEyeHeight, 0.0f};

    // Reached the far end.
    if (t_ >= 1.0f) {
        const iron::Vec3 f = farEndPoint(rope);
        exitFeet_ = iron::Vec3{f.x, 0.0f, f.z};
        return hasFooting(f.x, f.z, farIsland) ? Result::Won
                                               : Result::Dismounted;
    }
    // Retreated off the start end (only when actively walking back).
    if (t_ <= 0.0f && forward < 0.0f) {
        const iron::Vec3 m = mountEndPoint(rope);
        exitFeet_ = iron::Vec3{m.x, 0.0f, m.z};
        return Result::Dismounted;
    }
    return Result::Traversing;
}

iron::Vec3 RopeWalker::sampleRope(const iron::Rope& rope) const {
    const std::vector<iron::VerletPoint>& pts = rope.points();
    const int n = static_cast<int>(pts.size());
    if (n < 2) {
        return iron::Vec3{};
    }
    // t_ = 0 is the mounted end; map t_ onto the point list.
    const float param =
        atStart_ ? t_ * static_cast<float>(n - 1)
                 : (1.0f - t_) * static_cast<float>(n - 1);
    int i = static_cast<int>(param);
    if (i < 0) i = 0;
    if (i > n - 2) i = n - 2;
    const float frac = param - static_cast<float>(i);
    const iron::Vec3 p0 = pts[i].position;
    const iron::Vec3 p1 = pts[i + 1].position;
    return p0 + (p1 - p0) * frac;
}

iron::Vec3 RopeWalker::mountEndPoint(const iron::Rope& rope) const {
    const std::vector<iron::VerletPoint>& pts = rope.points();
    if (pts.empty()) {
        return iron::Vec3{};
    }
    return atStart_ ? pts.front().position : pts.back().position;
}

iron::Vec3 RopeWalker::farEndPoint(const iron::Rope& rope) const {
    const std::vector<iron::VerletPoint>& pts = rope.points();
    if (pts.empty()) {
        return iron::Vec3{};
    }
    return atStart_ ? pts.back().position : pts.front().position;
}

iron::Mat4 RopeWalker::viewMatrix() const {
    // Look direction from yaw/pitch (FirstPersonController convention).
    const float cp = std::cos(pitch_);
    const iron::Vec3 forward{
        -std::sin(yaw_) * cp,
        std::sin(pitch_),
        -std::cos(yaw_) * cp,
    };
    // Camera basis, then roll the up vector about the view axis by the lean.
    const iron::Vec3 worldUp{0.0f, 1.0f, 0.0f};
    const iron::Vec3 right = iron::normalize(iron::cross(forward, worldUp));
    const iron::Vec3 up = iron::cross(right, forward);
    const float roll = lean_ * kMaxRoll;
    const iron::Vec3 rolledUp =
        up * std::cos(roll) + right * std::sin(roll);
    return iron::lookAt(eye_, eye_ + forward, rolledUp);
}
