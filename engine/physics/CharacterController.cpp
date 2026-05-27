// CharacterController.cpp — Jolt CharacterVirtual wrapper (pimpl).

#include "physics/CharacterController.h"
#include "core/Log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

namespace iron {

namespace {

// Object layer for the character. Must match Layers::MOVING (= 1) in
// PhysicsWorld.cpp.
constexpr JPH::ObjectLayer kMovingLayer = 1;

inline JPH::Vec3 toJ(Vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
inline Vec3 toI(JPH::Vec3 v) { return Vec3{v.GetX(), v.GetY(), v.GetZ()}; }
// Note: JPH::RVec3 == JPH::Vec3 in single-precision builds, so a second
// overload would be a redefinition.

}  // namespace

// Bridges into PhysicsWorld's internal Jolt state. Defined in PhysicsWorld.cpp.
namespace internal {
JPH::PhysicsSystem*  getPhysicsSystem(PhysicsWorld& world);
JPH::TempAllocator*  getTempAllocator(PhysicsWorld& world);
}

struct CharacterController::Impl {
    JPH::Ref<JPH::CharacterVirtual> character;
    CharacterControllerConfig       cfg;
    PhysicsWorld*                   world  = nullptr;   // borrowed; used during update()
    JPH::PhysicsSystem*             system = nullptr;
};

CharacterController::CharacterController()  : impl_(std::make_unique<Impl>()) {}
CharacterController::~CharacterController() = default;

bool CharacterController::create(PhysicsWorld& world,
                                  const CharacterControllerConfig& cfg,
                                  Vec3 footPosition) {
    JPH::PhysicsSystem* sys = internal::getPhysicsSystem(world);
    if (!sys) {
        Log::error("CharacterController::create: PhysicsWorld not initialized");
        return false;
    }
    impl_->cfg    = cfg;
    impl_->world  = &world;
    impl_->system = sys;

    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mMaxSlopeAngle    = cfg.maxSlopeRad;
    settings->mShape            = new JPH::CapsuleShape(cfg.halfHeight, cfg.radius);
    settings->mInnerBodyLayer   = kMovingLayer;
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -cfg.radius);
    settings->mCharacterPadding = 0.02f;

    // CharacterVirtual position is the center of the capsule body.
    // Foot = center.y - (halfHeight + radius).
    const float centerY = footPosition.y + cfg.halfHeight + cfg.radius;
    impl_->character = new JPH::CharacterVirtual(
        settings,
        JPH::RVec3(footPosition.x, centerY, footPosition.z),
        JPH::Quat::sIdentity(),
        /*userData=*/0,
        sys);
    return true;
}

void CharacterController::destroy(PhysicsWorld&) {
    impl_->character = nullptr;  // JPH::Ref releases
    impl_->world     = nullptr;
    impl_->system    = nullptr;
}

void CharacterController::update(float dt, Vec3 desiredVelocity, bool wantJump, bool grounded) {
    if (!impl_->character || !impl_->system || !impl_->world) return;
    auto& ch = *impl_->character;

    JPH::Vec3 v = ch.GetLinearVelocity();
    v.SetX(desiredVelocity.x);
    v.SetZ(desiredVelocity.z);
    if (wantJump && grounded) {
        v.SetY(impl_->cfg.jumpVelocity);
    } else {
        v.SetY(v.GetY() + impl_->cfg.gravity * dt);
    }
    ch.SetLinearVelocity(v);

    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
    updateSettings.mWalkStairsStepUp        = JPH::Vec3(0, impl_->cfg.stepHeight, 0);
    updateSettings.mWalkStairsStepDownExtra = JPH::Vec3::sZero();

    JPH::TempAllocator* tempAlloc = internal::getTempAllocator(*impl_->world);
    if (!tempAlloc) return;

    ch.ExtendedUpdate(dt,
                      impl_->system->GetGravity(),
                      updateSettings,
                      impl_->system->GetDefaultBroadPhaseLayerFilter(kMovingLayer),
                      impl_->system->GetDefaultLayerFilter(kMovingLayer),
                      JPH::BodyFilter(),
                      JPH::ShapeFilter(),
                      *tempAlloc);
}

Vec3 CharacterController::footPosition() const {
    if (!impl_->character) return {};
    const JPH::RVec3 c = impl_->character->GetPosition();
    return Vec3{c.GetX(),
                c.GetY() - impl_->cfg.halfHeight - impl_->cfg.radius,
                c.GetZ()};
}

Vec3 CharacterController::velocity() const {
    if (!impl_->character) return {};
    return toI(impl_->character->GetLinearVelocity());
}

bool CharacterController::isGrounded() const {
    if (!impl_->character) return false;
    return impl_->character->GetGroundState() ==
        JPH::CharacterVirtual::EGroundState::OnGround;
}

void CharacterController::setFootPosition(Vec3 p) {
    if (!impl_->character) return;
    const float centerY = p.y + impl_->cfg.halfHeight + impl_->cfg.radius;
    impl_->character->SetPosition(JPH::RVec3(p.x, centerY, p.z));
}

void CharacterController::setVelocity(Vec3 v) {
    if (!impl_->character) return;
    impl_->character->SetLinearVelocity(toJ(v));
}

}  // namespace iron
