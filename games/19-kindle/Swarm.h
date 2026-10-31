#pragma once

// The player in Kindle: a point the swarm of sparks chases, and the room it flies
// through. There's no rendering here, so all of it can be tested without a GPU.
// The room is the same math as the Room and Pillars shapes in swarm.forge.json,
// which is how the player and every particle collide with the same world.

#include "math/Vec.h"

#include <algorithm>
#include <cmath>

namespace kindle {

using iron::Vec3;

struct Room {
    Vec3  half{20.0f, 6.0f, 20.0f};
    float spacing = 10.0f;     // Pillars sit at 5, 15, -5, -15 on each axis.
    float pillarHalf = 0.8f;
};

// Distance to the nearest surface: positive in open air, negative inside a wall.
inline float roomDistance(const Room& room, Vec3 p) {
    const float walls = -std::max({std::abs(p.x) - room.half.x, std::abs(p.y) - room.half.y,
                                   std::abs(p.z) - room.half.z});
    const auto cell = [&](float v) {
        return v - room.spacing * std::floor(v / room.spacing) - room.spacing * 0.5f;
    };
    const float dx = std::abs(cell(p.x)) - room.pillarHalf;
    const float dz = std::abs(cell(p.z)) - room.pillarHalf;
    const float ox = std::max(dx, 0.0f);
    const float oz = std::max(dz, 0.0f);
    const float pillars = std::sqrt(ox * ox + oz * oz) + std::min(std::max(dx, dz), 0.0f);
    return std::min(walls, pillars);
}

inline Vec3 roomNormal(const Room& room, Vec3 p) {
    constexpr float e = 0.02f;
    return iron::normalize(Vec3{
        roomDistance(room, {p.x + e, p.y, p.z}) - roomDistance(room, {p.x - e, p.y, p.z}),
        roomDistance(room, {p.x, p.y + e, p.z}) - roomDistance(room, {p.x, p.y - e, p.z}),
        roomDistance(room, {p.x, p.y, p.z + e}) - roomDistance(room, {p.x, p.y, p.z - e})});
}

struct Controls {
    Vec3 wish{};            // Direction to fly, up to length 1.
    bool squeeze = false;   // Narrow into a fast ribbon.
    bool ignite = false;    // Burst outward.
};

class Swarm {
public:
    static constexpr float kCruiseSpeed = 13.0f;
    static constexpr float kSqueezeSpeed = 26.0f;
    static constexpr float kCruiseSpread = 1.5f;
    static constexpr float kSqueezeSpread = 0.2f;
    static constexpr float kIgniteRest = 0.45f;  // Seconds between bursts.
    static constexpr float kMargin = 0.7f;       // Keeps the cloud off the walls.
    static constexpr float kIgniteCost = 0.34f;  // Two bursts and a bit from full heat.
    static constexpr float kHeatPerSecond = 0.16f;

    explicit Swarm(Vec3 start) : position_(start) {}

    void update(const Room& room, const Controls& controls, float dt) {
        const float s = 1.0f - std::exp(-dt * 7.0f);
        squeeze_ += ((controls.squeeze ? 1.0f : 0.0f) - squeeze_) * s;

        const float before = iron::length(velocity_);
        const float accel = 30.0f + 30.0f * squeeze_;
        velocity_ = velocity_ + controls.wish * (accel * dt);

        // Speed above the limit eases away, so that letting go of a squeeze sheds
        // speed rather than stalling.
        const float top = kCruiseSpeed + (kSqueezeSpeed - kCruiseSpeed) * squeeze_;
        const float limit = top + std::max(before - top, 0.0f) * std::exp(-dt * 5.0f);
        const float speed = iron::length(velocity_);
        if (speed > limit) velocity_ = velocity_ * (limit / speed);
        if (iron::length(controls.wish) < 0.01f) velocity_ = velocity_ * std::exp(-dt * 1.6f);

        position_ = position_ + velocity_ * dt;

        const float distance = roomDistance(room, position_) - kMargin;
        if (distance < 0.0f) {
            const Vec3 n = roomNormal(room, position_);
            position_ = position_ - n * distance;
            const float into = iron::dot(velocity_, n);
            if (into < 0.0f) velocity_ = velocity_ - n * (into * 1.3f);
        }

        rest_ = std::max(rest_ - dt, 0.0f);
        heat_ = std::min(heat_ + kHeatPerSecond * dt, 1.0f);
        if (controls.ignite && rest_ <= 0.0f && heat_ >= kIgniteCost) {
            rest_ = kIgniteRest;
            heat_ -= kIgniteCost;
            blast_ = 1.0f;
            flare_ = 1.0f;
        }
        phase_ += dt * (2.2f + iron::length(velocity_) * 0.18f);
        blast_ *= std::exp(-dt * 7.0f);
        flare_ *= std::exp(-dt * 2.2f);
    }

    Vec3  position() const { return position_; }
    Vec3  velocity() const { return velocity_; }
    float speed() const { return iron::length(velocity_); }
    float blast() const { return blast_; }    // 1 at a burst, gone in a moment.
    float flare() const { return flare_; }    // The same, but lingering, for light.
    float squeeze() const { return squeeze_; }
    float heat() const { return heat_; }      // What ignite spends, from 0 to 1.

    // A slow heartbeat, quicker when moving, with a beat's sharp rise and long fall.
    float pulse() const {
        const float beat = 0.5f + 0.5f * std::sin(phase_);
        return beat * beat * beat;
    }

    // Radius of the cloud. A burst throws its home outward, so it swells and returns.
    float spread() const {
        // It breathes when idle, and pulls in as it picks up speed.
        const float calm = 1.0f - std::min(speed() / kCruiseSpeed, 1.0f);
        const float breath = 1.0f + 0.22f * calm * pulse();
        const float rest = (kCruiseSpread + (kSqueezeSpread - kCruiseSpread) * squeeze_) * breath;
        return rest + flare_ * 3.5f;
    }

private:
    Vec3  position_;
    Vec3  velocity_{};
    float squeeze_ = 0.0f;
    float blast_ = 0.0f;
    float flare_ = 0.0f;
    float rest_ = 0.0f;
    float phase_ = 0.0f;
    float heat_ = 1.0f;
};

}  // namespace kindle
