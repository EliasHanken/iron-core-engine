#pragma once

// The player in Kindle: a point the swarm of sparks chases, and the room it flies
// through. There's no rendering here, so all of it can be tested without a GPU.
// The room is the same math as the Room and Pillars shapes in swarm.forge.json,
// which is how the player and every particle collide with the same world.

#include "math/Vec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace kindle {

using iron::Vec3;

struct Room {
    Vec3  half{20.0f, 6.0f, 20.0f};
    float spacing = 10.0f;     // Pillars sit at 5, 15, -5, -15 on each axis.
    float pillarHalf = 0.8f;

    // Beyond the hall: a narrow slit in the north wall, then the core's chamber.
    bool  sealed = true;
    float slitHalf = 1.0f;
    float slitHeight = 3.0f;
    float slitNear = -19.0f;
    float slitFar = -29.0f;
    Vec3  coreCenter{0.0f, 0.0f, -60.0f};
    float chamberRadius = 34.0f;
    float coreRadius = 8.0f;
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
    const float pillars = p.z < -20.5f ? 1000.0f
                                       : std::sqrt(ox * ox + oz * oz) + std::min(std::max(dx, dz), 0.0f);

    float air = walls;
    if (!room.sealed) {
        const float middle = (room.slitNear + room.slitFar) * 0.5f;
        const float slit = -std::max({std::abs(p.x) - room.slitHalf, std::abs(p.y) - room.slitHeight,
                                      std::abs(p.z - middle) - (room.slitNear - room.slitFar) * 0.5f});
        const float chamber = room.chamberRadius - iron::length(p - room.coreCenter);
        air = std::max({air, slit, chamber});
    }
    const float core = iron::length(p - room.coreCenter) - room.coreRadius;
    return std::min({air, pillars, core});
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
    static constexpr float kCruiseSpread = 0.7f;
    static constexpr float kSqueezeSpread = 0.1f;
    static constexpr float kIgniteRest = 0.45f;  // Seconds between bursts.
    static constexpr float kMargin = 1.15f;      // Keeps the cloud off the walls.
    static constexpr float kSqueezeMargin = 0.2f;  // A ribbon fits where a cloud can't.
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

        const float margin = kMargin + (kSqueezeMargin - kMargin) * squeeze_;
        const float distance = roomDistance(room, position_) - margin;
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
        ignited_ = blast_ == 1.0f;
        blast_ *= std::exp(-dt * 7.0f);
        flare_ *= std::exp(-dt * 2.2f);
    }

    // A burst nothing can stop, for the finale.
    void burst() {
        blast_ = 1.0f;
        flare_ = 1.0f;
    }

    Vec3  position() const { return position_; }
    Vec3  velocity() const { return velocity_; }
    float speed() const { return iron::length(velocity_); }
    float blast() const { return blast_; }    // 1 at a burst, gone in a moment.
    float flare() const { return flare_; }    // The same, but lingering, for light.
    float squeeze() const { return squeeze_; }
    float heat() const { return heat_; }      // What ignite spends, from 0 to 1.
    bool  ignited() const { return ignited_; } // True on the update of a burst.

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
        return rest + flare_ * 2.0f;
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
    bool  ignited_ = false;
};

// Where the hall has been relit. Each wake is a point of warmth that spreads out
// through the metal and stays. Igniting close to an old wake feeds it instead of
// starting another, so the player wakes the hall by going somewhere new.
class Wakes {
public:
    static constexpr std::size_t kMax = 12;      // Lights left after the swarm's and core's.
    static constexpr float kStartReach = 2.0f;
    static constexpr float kFullReach = 13.0f;
    static constexpr float kFeedReach = 4.0f;    // Growth from igniting on an old wake.
    static constexpr float kSpreadPerSecond = 1.6f;
    static constexpr float kMergeDistance = 7.0f;

    struct Wake {
        Vec3  position{};
        float target = 0.0f;   // How far it's spreading to.
        float reach = 0.0f;    // How far it's got, in metres.
    };

    void ignite(Vec3 position) {
        for (std::size_t i = 0; i < count_; ++i) {
            if (iron::length(wakes_[i].position - position) < kMergeDistance) {
                wakes_[i].target = std::min(wakes_[i].target + kFeedReach, kFullReach);
                return;
            }
        }
        if (count_ == kMax) return;
        wakes_[count_++] = {position, kFullReach * 0.7f, kStartReach};
    }

    void update(float dt) {
        for (std::size_t i = 0; i < count_; ++i) {
            wakes_[i].reach = std::min(wakes_[i].reach + kSpreadPerSecond * dt, wakes_[i].target);
        }
    }

    std::size_t count() const { return count_; }
    const Wake& operator[](std::size_t index) const { return wakes_[index]; }

    // How much of the hall is relit, from 0 to 1, read off a grid across the floor.
    float relit(const Room& room) const {
        constexpr int kCells = 12;
        int warm = 0;
        for (int x = 0; x < kCells; ++x) {
            for (int z = 0; z < kCells; ++z) {
                const float u = (static_cast<float>(x) + 0.5f) / kCells * 2.0f - 1.0f;
                const float v = (static_cast<float>(z) + 0.5f) / kCells * 2.0f - 1.0f;
                const Vec3 cell{u * room.half.x, 0.0f, v * room.half.z};
                for (std::size_t i = 0; i < count_; ++i) {
                    Vec3 flat = wakes_[i].position - cell;
                    flat.y = 0.0f;
                    if (iron::length(flat) < wakes_[i].reach) {
                        ++warm;
                        break;
                    }
                }
            }
        }
        return static_cast<float>(warm) / static_cast<float>(kCells * kCells);
    }

private:
    std::array<Wake, kMax> wakes_{};
    std::size_t count_ = 0;
};

// What a camera operator's body adds: shake from a hit, and leaning into turns.
class CameraFeel {
public:
    static constexpr float kMaxRoll = 0.22f;   // Radians.
    static constexpr float kShakeMetres = 0.32f;

    // A hit from 0 to 1. Hits add up, but only to a limit.
    void hit(float strength) { trauma_ = std::min(trauma_ + strength, 1.0f); }

    void update(float yawPerSecond, float dt) {
        time_ += dt;
        trauma_ = std::max(trauma_ - dt * 1.1f, 0.0f);
        const float want = std::clamp(-yawPerSecond * 0.09f, -kMaxRoll, kMaxRoll);
        roll_ += (want - roll_) * (1.0f - std::exp(-dt * 5.0f));
    }

    // Shake grows with the square of trauma, so small hits stay gentle.
    float shake() const { return trauma_ * trauma_; }
    float offsetX() const { return wave(31.0f, 47.0f) * shake() * kShakeMetres; }
    float offsetY() const { return wave(37.0f, 53.0f) * shake() * kShakeMetres; }
    float roll() const { return roll_ + wave(23.0f, 41.0f) * shake() * 0.05f; }

private:
    float wave(float a, float b) const { return 0.6f * std::sin(time_ * a) + 0.4f * std::sin(time_ * b + 1.3f); }

    float trauma_ = 0.0f;
    float roll_ = 0.0f;
    float time_ = 0.0f;
};

// The whole game: relight the hall, go through the gate, feed the core.
class Story {
public:
    static constexpr float kRelitToOpen = 0.55f;
    static constexpr float kGateSeconds = 4.0f;
    static constexpr float kFeedPerBurst = 0.26f;
    static constexpr float kFeedDistance = 24.0f;  // From the core's middle.
    static constexpr float kPullSeconds = 2.6f;    // The core drags you in, then blows.

    void update(const Room& room, Vec3 swarm, float relit, bool ignited, float dt) {
        if (relit >= kRelitToOpen) opening_ = true;
        if (opening_) gate_ = std::min(gate_ + dt / kGateSeconds, 1.0f);

        const bool nearCore = iron::length(swarm - room.coreCenter) < kFeedDistance;
        if (ignited && nearCore && !finale()) fed_ = std::min(fed_ + kFeedPerBurst, 1.0f);
        if (finale()) finaleSeconds_ += dt;
    }

    float gate() const { return gate_; }        // 0 shut, 1 open.
    bool  sealed() const { return gate_ < 0.5f; }
    float fed() const { return fed_; }          // How awake the core is.
    bool  finale() const { return fed_ >= 1.0f; }
    float finaleSeconds() const { return finaleSeconds_; }
    bool  pulling() const { return finale() && finaleSeconds_ < kPullSeconds; }
    float sinceIgnition() const { return finaleSeconds_ - kPullSeconds; }  // Below 0 before it.

private:
    bool  opening_ = false;
    float gate_ = 0.0f;
    float fed_ = 0.0f;
    float finaleSeconds_ = 0.0f;
};

}  // namespace kindle
