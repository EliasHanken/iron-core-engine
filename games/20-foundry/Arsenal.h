#pragma once

// What the player carries: the AK-47, an M1911 and a scoped M700, each its own
// view model (arms and gun, built in Blender) and its own feel (Gun). Switching
// puts the one in hand away, dropping it out of view, then draws the next with
// its own animation; nothing fires, aims or reloads in between.

#include "Gun.h"
#include "Models.h"
#include "ViewModel.h"
#include "core/Log.h"
#include "render/Renderer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace foundry {

struct Weapon {
    Arm         arm = Arm::Rifle;
    const char* label = "";      // On the HUD.
    float       aimFov = 0.0f;   // Vertical, radians, fully aimed.
    bool        scoped = false;  // Aimed, the view goes through a scope.
    float       flash = 1.0f;    // The muzzle flash's size.
    ViewModel   viewModel;
    Gun         gun;
};

class Arsenal {
public:
    static constexpr int kCount = 3;

    // Loads every weapon's view model from `dir`; false if any is missing.
    bool load(iron::Renderer& renderer, ModelLibrary& models, const std::string& dir) {
        constexpr float kDegrees = 3.14159265f / 180.0f;
        struct Spec {
            Arm arm;
            const char* label;
            const char* file;
            float aimFovDegrees;
            bool scoped;
            float flash;
            GunTuning tuning;
        };
        GunTuning rifle;  // The defaults are the AK's.

        GunTuning pistol;
        pistol.automatic = false;
        pistol.roundsPerMinute = 420.0f;  // As fast as a finger, no faster than the slide.
        pistol.magazine = 7;
        pistol.kickPitch = 0.016f;        // A sharp flip, and quickly back.
        pistol.kickYaw = 0.004f;
        pistol.recoverRate = 12.0f;
        pistol.kickBack = 0.9f;
        pistol.kickTilt = 3.6f;
        pistol.hipSpread = 0.012f;
        pistol.aimSpread = 0.003f;
        pistol.movingSpread = 0.008f;
        pistol.bloomPerShot = 0.005f;
        pistol.bloomDecay = 0.05f;
        pistol.aimInRate = 10.0f;

        GunTuning sniper;
        sniper.automatic = false;
        sniper.boltAction = true;
        sniper.roundsPerMinute = 60.0f;   // The bolt sets the pace.
        sniper.magazine = 5;
        sniper.perRound = true;
        sniper.kickPitch = 0.05f;         // A heavy shove that rocks the scope off target.
        sniper.kickYaw = 0.012f;
        sniper.recoverRate = 5.0f;
        sniper.recoverShare = 0.85f;
        sniper.kickBack = 2.4f;
        sniper.kickTilt = 5.0f;
        sniper.hipSpread = 0.045f;        // From the hip it's a guess.
        sniper.aimSpread = 0.0f;          // Through the scope, where the reticle is.
        sniper.movingSpread = 0.03f;
        sniper.bloomPerShot = 0.0f;
        sniper.aimInRate = 4.5f;          // A heavy rifle comes up slowly.

        const Spec specs[kCount] = {
            {Arm::Rifle, "AK-47", "ak47.gltf", 44.0f, false, 1.0f, rifle},
            {Arm::Pistol, "M1911", "m1911.gltf", 50.0f, false, 0.7f, pistol},
            {Arm::Sniper, "M700", "m700.gltf", 9.0f, true, 1.4f, sniper},
        };
        for (int i = 0; i < kCount; ++i) {
            Weapon& w = weapons_[i];
            const Spec& spec = specs[i];
            if (!w.viewModel.load(renderer, models, dir + spec.file)) return false;
            w.arm = spec.arm;
            w.label = spec.label;
            w.aimFov = spec.aimFovDegrees * kDegrees;
            w.scoped = spec.scoped;
            w.flash = spec.flash;
            w.gun = Gun(spec.tuning);
            // Each reload takes as long as its animation does.
            ViewModel& vm = w.viewModel;
            if (spec.tuning.perRound) {
                w.gun.setRoundReloadSeconds(vm.seconds("reload_start", 0.7f), vm.seconds("reload_round", 0.6f),
                                            vm.seconds("reload_end", 0.7f), vm.seconds("bolt", 0.85f));
            } else {
                w.gun.setReloadSeconds(vm.reloadSeconds(false, 1.9f), vm.reloadSeconds(true, 2.6f));
            }
        }
        return true;
    }

    Weapon& current() { return weapons_[current_]; }
    const Weapon& current() const { return weapons_[current_]; }
    int index() const { return current_; }

    // Takes a weapon in hand at once, drawing it (at the start).
    void equip(int i) {
        current_ = std::clamp(i, 0, kCount - 1);
        weapons_[current_].viewModel.redraw();
    }

    // Starts switching to weapon `i`: the one in hand goes away first.
    void select(int i) {
        if (i < 0 || i >= kCount || i == current_ || busy()) return;
        next_ = i;
        lowering_ = kLowerSeconds;
        weapons_[current_].gun.putAway();
    }
    // The next or previous weapon round the set (the mouse wheel).
    void cycle(int step) { select(((current_ + step) % kCount + kCount) % kCount); }

    // Advances a switch under way.
    void update(float dt) {
        if (lowering_ > 0.0f) {
            lowering_ = std::max(lowering_ - dt, 0.0f);
            if (lowering_ == 0.0f && next_ >= 0) {
                current_ = next_;
                next_ = -1;
                Weapon& w = weapons_[current_];
                w.viewModel.redraw();
                raising_ = std::min(w.viewModel.seconds("draw", 0.5f), kRaiseSeconds);
            }
        } else {
            raising_ = std::max(raising_ - dt, 0.0f);
        }
    }

    // How far the weapon in hand has dropped out of view while being put away, 0..1.
    float lowered() const {
        if (lowering_ <= 0.0f) return 0.0f;
        const float t = 1.0f - lowering_ / kLowerSeconds;
        return t * t * (3.0f - 2.0f * t);
    }
    // Switching: nothing fires, aims or reloads.
    bool busy() const { return lowering_ > 0.0f || raising_ > 0.0f; }

private:
    static constexpr float kLowerSeconds = 0.2f;
    static constexpr float kRaiseSeconds = 0.45f;

    std::array<Weapon, kCount> weapons_;
    int   current_ = 0;
    int   next_ = -1;
    float lowering_ = 0.0f;
    float raising_ = 0.0f;
};

}  // namespace foundry
