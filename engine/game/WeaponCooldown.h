#pragma once

#include <algorithm>

namespace iron {

// Monotonic cooldown gate. `nowSec` is the caller's clock — host uses
// its own monotonic clock; client uses the same. Reset jumps back to 0
// (next tryFire will succeed).
class WeaponCooldown {
public:
    explicit WeaponCooldown(float cooldownSec) : cooldown_(cooldownSec) {}

    bool tryFire(double nowSec) {
        if (nowSec < nextFireAt_) return false;
        nextFireAt_ = nowSec + cooldown_;
        return true;
    }

    float timeUntilReady(double nowSec) const {
        return std::max(0.0f, static_cast<float>(nextFireAt_ - nowSec));
    }

    void reset() { nextFireAt_ = 0.0; }

private:
    float cooldown_;
    double nextFireAt_ = 0.0;
};

}  // namespace iron
