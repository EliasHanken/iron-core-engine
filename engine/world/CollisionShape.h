#pragma once

#include "math/Vec.h"

namespace iron {

// What primitive the collider uses. Matches PhysicsWorld's shape set.
enum class ColliderShape { Box, Sphere, Capsule };

// Static = immovable collider (floors, walls). Dynamic = rigid body that
// falls / collides; its mesh transform is driven by physics during Play.
enum class ColliderBody { Static, Dynamic };

// Authorable collider on an entity. Built into a Jolt body on Edit->Play
// (host/sandbox owns the body lifetime); reset by M41 snapshot on Stop.
struct CollisionShape {
    ColliderShape shape       = ColliderShape::Box;
    ColliderBody  body        = ColliderBody::Static;
    Vec3          halfExtents = {0.5f, 0.5f, 0.5f};  // Box
    float         radius      = 0.5f;                // Sphere / Capsule
    float         halfHeight  = 0.5f;                // Capsule (cylinder half-height)
    float         mass        = 1.0f;                // Dynamic only
};

}  // namespace iron
