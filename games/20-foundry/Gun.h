#pragma once

// The rifle's feel, kept free of rendering and input so it can be tested: when it
// fires, how it kicks the view and the weapon, how it sways and bobs, and how
// wide its shots spread.

#include "math/Vec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace foundry {

using iron::Vec2;
using iron::Vec3;

// A spring pulled back to rest: the weapon's kick and sway both settle this way,
// quickly but with a little overshoot, like a held object.
struct Spring {
    float value = 0.0f;
    float velocity = 0.0f;

    void step(float dt, float stiffness, float damping) {
        velocity += (-stiffness * value - damping * velocity) * dt;
        value += velocity * dt;
    }
};

struct GunTuning {
    float roundsPerMinute = 660.0f;
    // Each shot lifts the view by this much, plus a little to one side.
    float kickPitch = 0.0085f;      // radians
    float kickYaw = 0.0030f;        // radians, random either way
    // Once the trigger lets go, the view drifts back to where it was aimed.
    float recoverRate = 9.0f;       // per second
    float recoverShare = 1.0f;
    // Spread: a cone around the aim, in radians.
    float hipSpread = 0.010f;
    float aimSpread = 0.0015f;
    float movingSpread = 0.012f;    // Added at full running speed.
    float airSpread = 0.03f;
    float bloomPerShot = 0.0028f;
    float bloomMax = 0.02f;
    float bloomDecay = 0.035f;      // radians per second
    float aimInRate = 7.5f;         // Aim blend per second.
    int   magazine = 30;
    float reloadSeconds = 1.9f;
};

// One random stream per gun, so that a recording plays back the same.
struct Random {
    std::uint32_t state = 0x9e3779b9u;
    float next() {  // 0..1
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
    }
    float signedNext() { return next() * 2.0f - 1.0f; }
};

class Gun {
public:
    explicit Gun(GunTuning tuning = {}) : tuning_(tuning), rounds_(tuning.magazine) {}

    // Advances the gun and returns how many shots left the barrel this step.
    // `look` is this step's mouse turn in radians (x yaw, y pitch), for sway;
    // `moving` is speed as a share of a run, 0..1.
    int update(float dt, bool trigger, bool aiming, bool reloadPressed, Vec2 look, float moving,
               bool grounded) {
        aim_ += ((aiming && !reloading()) ? 1.0f : -1.0f) * tuning_.aimInRate * dt;
        aim_ = std::clamp(aim_, 0.0f, 1.0f);
        moving_ = std::clamp(moving, 0.0f, 1.0f);
        grounded_ = grounded;

        if (reloadPressed && !reloading() && rounds_ < tuning_.magazine) reloadLeft_ = tuning_.reloadSeconds;
        if (reloading()) {
            reloadLeft_ = std::max(reloadLeft_ - dt, 0.0f);
            if (reloadLeft_ == 0.0f) rounds_ = tuning_.magazine;
        }

        int shots = 0;
        const float interval = 60.0f / tuning_.roundsPerMinute;
        if (trigger && !reloading() && rounds_ > 0) {
            cooldown_ -= dt;
            while (cooldown_ <= 0.0f && rounds_ > 0) {
                fire();
                ++shots;
                cooldown_ += interval;
            }
            held_ = true;
        } else {
            // Ready to fire the moment the trigger is pulled again, but never
            // faster than the action can cycle.
            cooldown_ = std::max(cooldown_ - dt, 0.0f);
            if (held_) {
                // Let go: the view comes most of the way back down.
                settleTarget_ = recoil_ * (1.0f - tuning_.recoverShare);
                held_ = false;
            }
            recoil_.x += (settleTarget_.x - recoil_.x) * std::min(tuning_.recoverRate * dt, 1.0f);
            recoil_.y += (settleTarget_.y - recoil_.y) * std::min(tuning_.recoverRate * dt, 1.0f);
            if (rounds_ == 0 && trigger && !reloading()) reloadLeft_ = tuning_.reloadSeconds;
        }

        bloom_ = std::max(bloom_ - tuning_.bloomDecay * dt, 0.0f);
        flash_ = std::max(flash_ - dt, 0.0f);

        // The weapon lags behind a turn, and kicks back along its own axis.
        swayX_.velocity -= look.x * 9.0f;
        swayY_.velocity -= look.y * 9.0f;
        swayX_.step(dt, 140.0f, 16.0f);
        swayY_.step(dt, 140.0f, 16.0f);
        kick_.step(dt, 320.0f, 22.0f);
        kickPitch_.step(dt, 260.0f, 18.0f);

        // A walking rhythm, faster and wider when running.
        if (grounded && moving > 0.05f) bobPhase_ += dt * (7.0f + 3.5f * moving_);
        bobAmount_ += ((grounded ? moving_ : 0.0f) - bobAmount_) * std::min(dt * 8.0f, 1.0f);
        return shots;
    }

    // The view's offset from the player's aim, from recoil: x yaw, y pitch (up).
    Vec2 viewKick() const { return recoil_; }

    // A random direction inside the current spread, as yaw and pitch offsets.
    Vec2 shotDeviation() {
        const float angle = random_.next() * 6.2831853f;
        const float radius = std::sqrt(random_.next()) * spread();
        return {std::cos(angle) * radius, std::sin(angle) * radius};
    }

    float spread() const {
        const float base = tuning_.hipSpread + (tuning_.aimSpread - tuning_.hipSpread) * aim_;
        const float move = tuning_.movingSpread * moving_ * (1.0f - 0.6f * aim_);
        return base + move + (grounded_ ? 0.0f : tuning_.airSpread) + bloom_;
    }

    // How far the weapon has come up to the eye, 0..1.
    float aim() const { return aim_; }
    // Seconds of muzzle flash left.
    float flash() const { return flash_; }
    int rounds() const { return rounds_; }
    bool reloading() const { return reloadLeft_ > 0.0f; }
    // 0 at the start of a reload, 1 when it's done.
    float reloadProgress() const {
        return reloading() ? 1.0f - reloadLeft_ / tuning_.reloadSeconds : 1.0f;
    }
    const GunTuning& tuning() const { return tuning_; }

    // Where the weapon sits in view space (x right, y up, z towards the eye), and
    // how it's tilted (pitch, yaw, roll), from all of the above.
    Vec3 viewOffset() const {
        // Where the bottom of the pistol grip sits. Aimed, the line through the rear
        // notch and the front post (0.185 m above the grip) passes through the eye. As
        // in most shooters, the rifle sits further out than a real cheek weld would put
        // it, so the stock (0.26 m behind the grip) doesn't fill the bottom of the view.
        const Vec3 hip{0.13f, -0.27f, -0.27f};
        const Vec3 aimed{0.0f, -0.185f, -0.34f};
        const float a = smooth(aim_);
        Vec3 at{hip.x + (aimed.x - hip.x) * a, hip.y + (aimed.y - hip.y) * a, hip.z + (aimed.z - hip.z) * a};
        const float bob = bobAmount_ * (1.0f - 0.8f * a);
        at.x += std::sin(bobPhase_) * 0.010f * bob + swayX_.value * 0.05f * (1.0f - 0.7f * a);
        at.y += -std::abs(std::cos(bobPhase_)) * 0.012f * bob + swayY_.value * 0.05f * (1.0f - 0.7f * a);
        at.z += kick_.value;
        // A reload dips the weapon out of the way and brings it back.
        if (reloading()) {
            const float t = reloadProgress();
            at.y -= std::sin(t * 3.1415927f) * 0.09f;
        }
        return at;
    }
    Vec3 viewTilt() const {
        float roll = swayX_.value * 0.8f;
        if (reloading()) roll += std::sin(reloadProgress() * 3.1415927f) * 0.6f;
        return {kickPitch_.value + swayY_.value * 0.4f, swayX_.value * 0.4f, roll};
    }

private:
    static float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

    void fire() {
        --rounds_;
        recoil_.y += tuning_.kickPitch * (1.0f - 0.35f * aim_);
        recoil_.x += random_.signedNext() * tuning_.kickYaw;
        bloom_ = std::min(bloom_ + tuning_.bloomPerShot, tuning_.bloomMax);
        kick_.velocity += 1.1f * (1.0f - 0.5f * aim_);
        kickPitch_.velocity += 2.2f * (1.0f - 0.5f * aim_);
        flash_ = 0.05f;
    }

    GunTuning tuning_;
    Random    random_;
    int       rounds_ = 30;
    float     reloadLeft_ = 0.0f;
    float     cooldown_ = 0.0f;
    bool      held_ = false;
    Vec2      recoil_{0.0f, 0.0f};
    Vec2      settleTarget_{0.0f, 0.0f};
    float     bloom_ = 0.0f;
    float     flash_ = 0.0f;
    float     aim_ = 0.0f;
    float     moving_ = 0.0f;
    bool      grounded_ = true;
    Spring    swayX_;
    Spring    swayY_;
    Spring    kick_;
    Spring    kickPitch_;
    float     bobPhase_ = 0.0f;
    float     bobAmount_ = 0.0f;
};

}  // namespace foundry
