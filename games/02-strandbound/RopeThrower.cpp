#include "RopeThrower.h"

#include "math/Ray.h"

namespace {
constexpr float kChargeTime = 1.0f;      // seconds to reach full charge
constexpr float kMinThrowSpeed = 9.0f;   // units/sec at zero charge
constexpr float kMaxThrowSpeed = 28.0f;  // units/sec at full charge
constexpr float kGravity = -20.0f;       // projectile gravity, units/sec^2
constexpr float kKillY = -25.0f;         // below this, the throw has failed
}  // namespace

RopeThrower::Event RopeThrower::update(bool throwHeld, bool hasRope,
                                       iron::Vec3 eye, iron::Vec3 lookDir,
                                       iron::Vec3 feet,
                                       const std::vector<iron::Aabb>& colliders,
                                       float dt) {
    // Re-arm once the button is released, so holding it does not chain throws.
    if (!throwHeld) {
        armed_ = true;
    }

    switch (state_) {
        case State::Idle:
            if (throwHeld && hasRope && armed_) {
                state_ = State::Charging;
                charge_ = 0.0f;
            }
            return Event::None;

        case State::Charging:
            if (throwHeld) {
                charge_ += dt / kChargeTime;
                if (charge_ > 1.0f) {
                    charge_ = 1.0f;
                }
                return Event::None;
            }
            // Button released — launch.
            armed_ = false;
            nearEnd_ = feet;
            projectilePos_ = eye;
            projectileVel_ =
                lookDir * (kMinThrowSpeed +
                           (kMaxThrowSpeed - kMinThrowSpeed) * charge_);
            state_ = State::InFlight;
            return Event::None;

        case State::InFlight: {
            projectileVel_.y += kGravity * dt;
            const iron::Vec3 next = projectilePos_ + projectileVel_ * dt;

            // Test the step's travel segment against the world boxes.
            const iron::Vec3 delta = next - projectilePos_;
            const float segLen = iron::length(delta);
            if (segLen > 1e-6f) {
                const iron::Ray ray{projectilePos_, delta * (1.0f / segLen)};
                float bestT = 1e30f;
                bool hit = false;
                for (const iron::Aabb& box : colliders) {
                    float t = 0.0f;
                    if (iron::intersectRayAabb(ray, box, t) && t <= segLen &&
                        t < bestT) {
                        bestT = t;
                        hit = true;
                    }
                }
                if (hit) {
                    farEnd_ = projectilePos_ + ray.direction * bestT;
                    state_ = State::Idle;
                    charge_ = 0.0f;
                    return Event::Landed;
                }
            }

            projectilePos_ = next;
            if (projectilePos_.y < kKillY) {
                state_ = State::Idle;
                charge_ = 0.0f;
                return Event::Missed;
            }
            return Event::None;
        }
    }
    return Event::None;
}
