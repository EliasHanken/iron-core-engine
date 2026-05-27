#pragma once

#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Vec.h"

#include <cstdint>
#include <memory>

namespace iron {

// Opaque handles. Game code stores these but does not inspect them.
struct BodyId {
    std::uint32_t value = 0;
    bool isValid() const { return value != 0; }
};
struct JointId {
    std::uint32_t value = 0;
    bool isValid() const { return value != 0; }
};

inline constexpr BodyId  kInvalidBody  {};
inline constexpr JointId kInvalidJoint {};

inline bool operator==(BodyId  a, BodyId  b) { return a.value == b.value; }
inline bool operator==(JointId a, JointId b) { return a.value == b.value; }

// Thin engine wrapper around Jolt Physics. Hides all Jolt types from
// public headers via pimpl: game code includes only this header and the
// engine math types it already uses.
//
// Threading: PhysicsWorld is single-threaded externally — call `step`
// from one thread. The internal Jolt job system runs on its own thread.
//
// Determinism: configured to be byte-deterministic across runs and
// machines on the same architecture. step(dt) with identical bodies +
// dt produces identical body states.
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    bool init();
    void shutdown();

    // Deterministic fixed step. Caller picks dt (typically 1/60).
    void step(float dt);

    // --- Body creation ---
    BodyId createStaticBox     (Vec3 pos, Vec3 halfExtents);
    BodyId createDynamicBox    (Vec3 pos, Vec3 halfExtents, float mass);
    BodyId createDynamicSphere (Vec3 pos, float radius,         float mass);
    BodyId createDynamicCapsule(Vec3 pos, float halfHeight, float radius, float mass);
    void   destroyBody(BodyId);

    // --- Body state ---
    Vec3 bodyPosition (BodyId) const;
    Quat bodyRotation (BodyId) const;
    Mat4 bodyTransform(BodyId) const;
    bool isBodyAlive  (BodyId) const;

    // --- Forces / impulses ---
    void applyImpulse     (BodyId, Vec3 impulse);
    void applyForceAtPoint(BodyId, Vec3 force, Vec3 worldPoint);
    void setVelocity      (BodyId, Vec3 vel);

    // --- Queries ---
    struct RaycastHit {
        bool   hit    = false;
        BodyId body   = kInvalidBody;
        Vec3   point  {};
        Vec3   normal {};
        float  t      = 0.0f;
    };
    RaycastHit raycast(Vec3 origin, Vec3 direction, float maxDistance) const;

    // --- Joints ---
    // Swing-twist: spine, neck, shoulders, hips. World-space pivot +
    // twist axis. `swingLimit` is the cone half-angle the twist axis can
    // swing through; `twistLimit` is rotation around the twist axis.
    // Both in radians.
    JointId createSwingTwistJoint(BodyId a, BodyId b,
                                  Vec3 pivotWorld, Vec3 twistAxisWorld,
                                  float swingLimitRad, float twistLimitRad);

    // Hinge: elbow, knee. World-space pivot + hinge axis + signed angle
    // limits in radians.
    JointId createHingeJoint(BodyId a, BodyId b,
                             Vec3 pivotWorld, Vec3 hingeAxisWorld,
                             float minAngleRad, float maxAngleRad);

    void destroyJoint(JointId);

    // Internal pimpl — exposed in header so other engine TUs (Ragdoll, etc.)
    // can reach Jolt internals via friendship. Game code never touches Impl.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace iron
