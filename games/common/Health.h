#pragma once

#include <algorithm>

namespace iron {

struct Health {
    int max = 100;
    int current = 100;
};

inline bool isAlive(const Health& h) { return h.current > 0; }

inline void applyDamage(Health& h, int dmg) {
    // Negative damage is silently ignored — engine refuses to heal via
    // applyDamage. Use resetHealth to fully refill.
    h.current = std::max(0, h.current - std::max(0, dmg));
}

inline void resetHealth(Health& h) { h.current = h.max; }

}  // namespace iron
