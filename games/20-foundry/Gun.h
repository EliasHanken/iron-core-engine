#pragma once

// A gun's feel, kept free of rendering and input so it can be tested: when it
// fires (full-auto, a shot a pull, or a bolt worked between shots), how it reloads
// (a magazine, or a round at a time), how it kicks the view and the weapon, how it
// sways and bobs, and how wide its shots spread.

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
    bool  automatic = true;         // Off: one shot per pull of the trigger.
    bool  boltAction = false;       // Each shot then needs the bolt worked.
    float boltSeconds = 0.85f;      // Working it, from the hand leaving the grip to ready again.
    float boltDelay = 0.16f;        // From the shot to the hand leaving the grip.
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
    // How hard each shot shoves the weapon back and tips it up (spring velocities).
    float kickBack = 1.1f;
    float kickTilt = 2.2f;
    // The camera's punch on each shot, springing back: a nod up and a roll either
    // way (peak radians), and the view widening a touch (radians of field of view).
    float punchPitch = 0.004f;
    float punchRoll = 0.004f;
    float punchFov = 0.0f;
    int   magazine = 30;
    float reloadSeconds = 1.9f;
    float reloadEmptySeconds = 2.6f;  // With the bolt to rack.
    // A round at a time instead (a bolt-action's internal magazine): open, one
    // round after another until full or the trigger's pulled, close.
    bool  perRound = false;
    float reloadStartSeconds = 0.7f;
    float reloadRoundSeconds = 0.6f;
    float reloadEndSeconds = 0.7f;
};

// Which of the player's guns: each sounds and handles its own way.
enum class Arm : std::uint8_t { Rifle, Pistol, Sniper };

// What the gun's animation should start playing, read once by the game.
enum class GunCue : std::uint8_t { None, Reload, ReloadEmpty, ReloadStart, ReloadRound, ReloadEnd, Bolt };

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
        // Working the bolt takes the eye off the sights, as reloading does.
        aim_ += ((aiming && !reloading() && !boltCycling()) ? 1.0f : -1.0f) * tuning_.aimInRate * dt;
        aim_ = std::clamp(aim_, 0.0f, 1.0f);
        moving_ = std::clamp(moving, 0.0f, 1.0f);
        grounded_ = grounded;
        const bool pulled = trigger && !triggerWasDown_;
        triggerWasDown_ = trigger;

        if (reloadPressed && !reloading() && rounds_ < tuning_.magazine) startReload();
        if (reloading()) updateReload(dt, pulled);
        if (boltDelay_ > 0.0f) {
            boltDelay_ = std::max(boltDelay_ - dt, 0.0f);
            if (boltDelay_ == 0.0f) {
                cue_ = GunCue::Bolt;
                boltLeft_ = tuning_.boltSeconds;
            }
        } else {
            boltLeft_ = std::max(boltLeft_ - dt, 0.0f);
        }

        int shots = 0;
        const float interval = 60.0f / tuning_.roundsPerMinute;
        // A semi-automatic or a bolt-action fires once a pull; it waits for a release.
        const bool free = tuning_.automatic || !shotThisPull_;
        if (trigger && !reloading() && !boltCycling() && rounds_ > 0 && free) {
            cooldown_ -= dt;
            while (cooldown_ <= 0.0f && rounds_ > 0) {
                fire();
                ++shots;
                cooldown_ += interval;
                shotThisPull_ = true;
                if (!tuning_.automatic) break;
            }
            if (tuning_.boltAction && shots > 0 && rounds_ > 0) boltDelay_ = tuning_.boltDelay;
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
            if (rounds_ == 0 && trigger && !reloading()) startReload();
        }
        if (!trigger) shotThisPull_ = false;

        bloom_ = std::max(bloom_ - tuning_.bloomDecay * dt, 0.0f);
        flash_ = std::max(flash_ - dt, 0.0f);

        // The weapon lags behind a turn, and kicks back along its own axis.
        swayX_.velocity -= look.x * 9.0f;
        swayY_.velocity -= look.y * 9.0f;
        swayX_.step(dt, 140.0f, 16.0f);
        swayY_.step(dt, 140.0f, 16.0f);
        kick_.step(dt, 320.0f, 22.0f);
        kickPitch_.step(dt, 260.0f, 18.0f);
        punchPitch_.step(dt, 420.0f, 26.0f);
        punchRoll_.step(dt, 300.0f, 20.0f);
        punchFov_.step(dt, 260.0f, 24.0f);

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
    bool reloading() const { return reloadLeft_ > 0.0f || phase_ != Phase::None; }
    // 0 at the start of a reload, 1 when it's done (a round at a time: how full).
    float reloadProgress() const {
        if (phase_ != Phase::None) return static_cast<float>(rounds_) / static_cast<float>(tuning_.magazine);
        return reloading() ? 1.0f - reloadLeft_ / reloadLength_ : 1.0f;
    }
    // Whether the reload under way started from an empty magazine.
    bool reloadingEmpty() const { return reloading() && reloadEmpty_; }
    // Whether the bolt's being worked (or is about to be) after a shot.
    bool boltCycling() const { return boltDelay_ > 0.0f || boltLeft_ > 0.0f; }
    // What the animation should start now, if anything; reading it clears it.
    GunCue takeCue() {
        const GunCue c = cue_;
        cue_ = GunCue::None;
        return c;
    }
    // Puts the gun away mid-whatever: a reload abandoned (rounds already in stay), a
    // bolt left to work once it's next drawn and up.
    void putAway() {
        reloadLeft_ = 0.0f;
        phase_ = Phase::None;
        if (boltCycling()) {
            boltDelay_ = tuning_.boltDelay + kDrawSeconds;
            boltLeft_ = 0.0f;
        }
        aim_ = 0.0f;
        cue_ = GunCue::None;
    }
    const GunTuning& tuning() const { return tuning_; }

    // How far the weapon is moved from where its animation holds it, in view space
    // (x right, y up, z towards the eye), and how it's tilted (pitch, yaw, roll):
    // the kick, sway and bob, which play on top of the hip and aimed poses.
    Vec3 viewOffset() const {
        const float a = smooth(aim_);
        Vec3 at{0.0f, 0.0f, 0.0f};
        const float bob = bobAmount_ * (1.0f - 0.8f * a);
        at.x += std::sin(bobPhase_) * 0.010f * bob + swayX_.value * 0.05f * (1.0f - 0.7f * a);
        at.y += -std::abs(std::cos(bobPhase_)) * 0.012f * bob + swayY_.value * 0.05f * (1.0f - 0.7f * a);
        at.z += kick_.value;
        return at;
    }
    // The camera's punch from the last shots: pitch up, roll, and wider field of view.
    Vec3 viewPunch() const { return {punchPitch_.value, punchRoll_.value, punchFov_.value}; }

    Vec3 viewTilt() const {
        const float roll = swayX_.value * 0.8f;
        return {kickPitch_.value + swayY_.value * 0.4f, swayX_.value * 0.4f, roll};
    }

    // How long a reload takes, from a part-used magazine and from an empty one
    // (which has the bolt to rack): the animation's lengths.
    void setReloadSeconds(float partial, float empty) {
        tuning_.reloadSeconds = partial;
        tuning_.reloadEmptySeconds = empty;
    }
    // A round at a time: opening, each round, closing; and working the bolt.
    void setRoundReloadSeconds(float start, float round, float end, float bolt) {
        tuning_.reloadStartSeconds = start;
        tuning_.reloadRoundSeconds = round;
        tuning_.reloadEndSeconds = end;
        tuning_.boltSeconds = bolt;
    }

private:
    static float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

    enum class Phase : std::uint8_t { None, Start, Round, End };
    static constexpr float kDrawSeconds = 0.65f;  // About as long as a switch takes.

    void startReload() {
        reloadEmpty_ = rounds_ == 0;
        boltDelay_ = 0.0f;
        boltLeft_ = 0.0f;
        if (tuning_.perRound) {
            phase_ = Phase::Start;
            phaseLeft_ = tuning_.reloadStartSeconds;
            stopAfterRound_ = false;
            cue_ = GunCue::ReloadStart;
            return;
        }
        reloadLength_ = reloadEmpty_ ? tuning_.reloadEmptySeconds : tuning_.reloadSeconds;
        reloadLeft_ = reloadLength_;
        cue_ = reloadEmpty_ ? GunCue::ReloadEmpty : GunCue::Reload;
    }

    void updateReload(float dt, bool pulled) {
        if (phase_ == Phase::None) {
            reloadLeft_ = std::max(reloadLeft_ - dt, 0.0f);
            if (reloadLeft_ == 0.0f) rounds_ = tuning_.magazine;
            return;
        }
        // A round at a time: a pull of the trigger finishes the round in hand and
        // closes up, to shoot sooner.
        if (pulled && rounds_ > 0) stopAfterRound_ = true;
        phaseLeft_ -= dt;
        if (phaseLeft_ > 0.0f) return;
        switch (phase_) {
            case Phase::Start:
                phase_ = Phase::Round;
                phaseLeft_ += tuning_.reloadRoundSeconds;
                cue_ = GunCue::ReloadRound;
                break;
            case Phase::Round:
                ++rounds_;
                if (rounds_ >= tuning_.magazine || stopAfterRound_) {
                    phase_ = Phase::End;
                    phaseLeft_ += tuning_.reloadEndSeconds;
                    cue_ = GunCue::ReloadEnd;
                } else {
                    phaseLeft_ += tuning_.reloadRoundSeconds;
                    cue_ = GunCue::ReloadRound;
                }
                break;
            case Phase::End:
            case Phase::None:
                phase_ = Phase::None;
                phaseLeft_ = 0.0f;
                break;
        }
    }

    void fire() {
        --rounds_;
        recoil_.y += tuning_.kickPitch * (1.0f - 0.35f * aim_);
        recoil_.x += random_.signedNext() * tuning_.kickYaw;
        bloom_ = std::min(bloom_ + tuning_.bloomPerShot, tuning_.bloomMax);
        kick_.velocity += tuning_.kickBack * (1.0f - 0.5f * aim_);
        kickPitch_.velocity += tuning_.kickTilt * (1.0f - 0.5f * aim_);
        // Each spring peaks at its tuned value: velocity per radian of peak, for
        // its stiffness and damping, stepped at 60 Hz.
        punchPitch_.velocity += tuning_.punchPitch * 69.0f;
        punchRoll_.velocity += tuning_.punchRoll * 48.0f * random_.signedNext();
        punchFov_.velocity += tuning_.punchFov * 58.0f;
        flash_ = 0.05f;
    }

    GunTuning tuning_;
    Random    random_;
    int       rounds_ = 30;
    float     reloadLeft_ = 0.0f;
    float     reloadLength_ = 1.0f;
    bool      reloadEmpty_ = false;
    Phase     phase_ = Phase::None;
    float     phaseLeft_ = 0.0f;
    bool      stopAfterRound_ = false;
    GunCue    cue_ = GunCue::None;
    float     boltDelay_ = 0.0f;
    float     boltLeft_ = 0.0f;
    float     cooldown_ = 0.0f;
    bool      held_ = false;
    bool      triggerWasDown_ = false;
    bool      shotThisPull_ = false;
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
    Spring    punchPitch_;
    Spring    punchRoll_;
    Spring    punchFov_;
    float     bobPhase_ = 0.0f;
    float     bobAmount_ = 0.0f;
};

}  // namespace foundry
