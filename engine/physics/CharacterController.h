#pragma once

#include "math/Vec.h"
#include "physics/PhysicsWorld.h"

#include <memory>

namespace iron {

struct CharacterControllerConfig {
    float radius        = 0.30f;
    float halfHeight    = 0.90f;        // capsule body (excluding hemispheres);
                                         // total height = 2 * (halfHeight + radius) = 2.4m
    float maxSlopeRad   = 0.785398f;    // 45 deg
    float stepHeight    = 0.30f;
    float jumpVelocity  = 5.5f;
    float gravity       = -9.81f;
};

// Capsule character controller wrapping JPH::CharacterVirtual. One
// instance owns one CharacterVirtual registered in a PhysicsWorld.
//
// Per-tick contract:
//   1. (optional) setFootPosition / setVelocity — used by reconciliation replay
//   2. update(dt, desiredHorizontalVelocity, wantJump)
//   3. PhysicsWorld::step(dt) to advance the rest of the world
//   4. read footPosition / velocity / isGrounded
//
// The controller applies gravity inside update() — the rest of the
// world's gravity is applied by PhysicsWorld::step.
class CharacterController {
public:
    CharacterController();
    ~CharacterController();

    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    // Creates the CharacterVirtual in `world`. `footPosition` is the
    // bottom of the capsule.
    bool create(PhysicsWorld& world,
                const CharacterControllerConfig& cfg,
                Vec3 footPosition);

    // Destroys the CharacterVirtual. Safe to call multiple times.
    void destroy(PhysicsWorld& world);

    // Per-tick. `desiredVelocity` is the world-space horizontal velocity
    // (x and z; y is ignored — gravity owns y). `wantJump` is one-shot:
    // sets vy = jumpVelocity if grounded, ignored otherwise.
    void update(float dt, Vec3 desiredVelocity, bool wantJump);

    // State accessors.
    Vec3 footPosition() const;
    Vec3 velocity()     const;
    bool isGrounded()   const;

    // State mutators. Used for reconciliation replay in PredictionEngine.
    void setFootPosition(Vec3);
    void setVelocity(Vec3);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iron
