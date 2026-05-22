#pragma once

#include "math/Aabb.h"
#include "math/Vec.h"

#include <vector>

// Strandbound's rope-throwing mechanic. The player charges a throw and
// releases; the rope's far end flies as a projectile under gravity and sticks
// where it lands. Game-specific gameplay — it lives with the game, not the
// engine. Nothing here touches OpenGL, so it is headless and unit-testable.
class RopeThrower {
public:
    enum class State { Idle, Charging, InFlight };

    // What happened on a given update step.
    enum class Event { None, Landed, Missed };

    // Advance one fixed step.
    //   throwHeld  - is the throw button down this step
    //   hasRope    - is a rope available to throw (the pool is non-empty)
    //   eye        - the player's eye position (the projectile launch point)
    //   lookDir    - the player's look direction (unit length)
    //   feet       - the player's feet position (the rope's near end on a throw)
    //   colliders  - the world boxes the projectile can stick to
    // Returns Landed (a rope should be created from ropeNearEnd to ropeFarEnd),
    // Missed (the throw failed), or None.
    [[nodiscard]] Event update(bool throwHeld, bool hasRope, iron::Vec3 eye,
                               iron::Vec3 lookDir, iron::Vec3 feet,
                               const std::vector<iron::Aabb>& colliders,
                               float dt);

    State state() const { return state_; }
    float charge() const { return charge_; }  // 0..1, for the HUD bar
    iron::Vec3 projectilePosition() const { return projectilePos_; }

    // Valid after an update that returned Event::Landed.
    iron::Vec3 ropeNearEnd() const { return nearEnd_; }
    iron::Vec3 ropeFarEnd() const { return farEnd_; }

private:
    State state_ = State::Idle;
    float charge_ = 0.0f;
    bool armed_ = true;  // false from launch until the button is released

    iron::Vec3 projectilePos_{};
    iron::Vec3 projectileVel_{};
    iron::Vec3 nearEnd_{};
    iron::Vec3 farEnd_{};
};
