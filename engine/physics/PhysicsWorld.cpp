// PhysicsWorld.cpp — Jolt Physics wrapper (pimpl).
// All Jolt types live in this translation unit. Public header exposes
// only engine math types + opaque handles.

#include "physics/PhysicsWorld.h"
#include "core/Log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/RegisterTypes.h>

namespace iron {

namespace {

namespace Layers {
    constexpr JPH::ObjectLayer NON_MOVING = 0;
    constexpr JPH::ObjectLayer MOVING     = 1;
    constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}
namespace BroadphaseLayers {
    constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    constexpr JPH::BroadPhaseLayer MOVING(1);
    constexpr JPH::uint NUM_LAYERS = 2;
}

class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BroadPhaseLayerInterfaceImpl() {
        mTable[Layers::NON_MOVING] = BroadphaseLayers::NON_MOVING;
        mTable[Layers::MOVING]     = BroadphaseLayers::MOVING;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadphaseLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return mTable[inLayer];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "Iron"; }
#endif
private:
    JPH::BroadPhaseLayer mTable[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer obj, JPH::BroadPhaseLayer bp) const override {
        switch (obj) {
            case Layers::NON_MOVING: return bp == BroadphaseLayers::MOVING;
            case Layers::MOVING:     return true;
            default: return false;
        }
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == Layers::NON_MOVING && b == Layers::NON_MOVING) return false;
        return true;
    }
};

inline JPH::Vec3 toJ(Vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
inline Vec3      toI(JPH::Vec3 v) { return Vec3{v.GetX(), v.GetY(), v.GetZ()}; }
inline JPH::Quat toJ(Quat q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
inline Quat      toI(JPH::Quat q) { return Quat{q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

inline Mat4 mat4FromJolt(JPH::RVec3 pos, JPH::Quat rot) {
    JPH::Mat44 m = JPH::Mat44::sRotationTranslation(rot, pos);
    Mat4 out;
    for (int col = 0; col < 4; ++col) {
        JPH::Vec4 c = m.GetColumn4(col);
        out.at(0, col) = c.GetX();
        out.at(1, col) = c.GetY();
        out.at(2, col) = c.GetZ();
        out.at(3, col) = c.GetW();
    }
    return out;
}

bool g_joltInitialized = false;

inline JPH::BodyID toJoltBodyId(BodyId b) {
    return JPH::BodyID(b.value);
}

}  // namespace

struct PhysicsWorld::Impl {
    JPH::TempAllocatorImpl*     temp       = nullptr;
    JPH::JobSystemThreadPool*   jobs       = nullptr;
    JPH::PhysicsSystem*         system     = nullptr;
    BroadPhaseLayerInterfaceImpl       broadphaseLayers;
    ObjectVsBroadPhaseLayerFilterImpl  objectVsBroadphase;
    ObjectLayerPairFilterImpl          objectVsObject;
};

PhysicsWorld::PhysicsWorld() : impl_(std::make_unique<Impl>()) {}
PhysicsWorld::~PhysicsWorld() { shutdown(); }

bool PhysicsWorld::init() {
    if (!g_joltInitialized) {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        g_joltInitialized = true;
    }

    impl_->temp = new JPH::TempAllocatorImpl(10 * 1024 * 1024);

    // Single thread for byte-deterministic results.
    const int threadCount = 1;
    impl_->jobs = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs,
                                                JPH::cMaxPhysicsBarriers,
                                                threadCount);

    impl_->system = new JPH::PhysicsSystem();
    impl_->system->Init(
        4096, 0, 4096, 2048,
        impl_->broadphaseLayers,
        impl_->objectVsBroadphase,
        impl_->objectVsObject);
    impl_->system->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

    Log::info("PhysicsWorld: Jolt initialized (deterministic single-threaded mode)");
    return true;
}

void PhysicsWorld::shutdown() {
    if (!impl_) return;
    if (impl_->system) { delete impl_->system; impl_->system = nullptr; }
    if (impl_->jobs)   { delete impl_->jobs;   impl_->jobs   = nullptr; }
    if (impl_->temp)   { delete impl_->temp;   impl_->temp   = nullptr; }
}

void PhysicsWorld::step(float dt) {
    if (!impl_->system) return;
    constexpr int kCollisionSteps = 1;
    impl_->system->Update(dt, kCollisionSteps, impl_->temp, impl_->jobs);
}

namespace {

BodyId createBodyImpl(PhysicsWorld::Impl& impl,
                      JPH::ShapeRefC shape,
                      JPH::RVec3 pos,
                      JPH::EMotionType motion,
                      JPH::ObjectLayer layer,
                      float mass) {
    JPH::BodyCreationSettings settings(shape, pos, JPH::Quat::sIdentity(), motion, layer);
    if (motion == JPH::EMotionType::Dynamic) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = mass;
    }
    JPH::BodyInterface& bi = impl.system->GetBodyInterface();
    JPH::BodyID jid = bi.CreateAndAddBody(settings,
        motion == JPH::EMotionType::Dynamic ? JPH::EActivation::Activate
                                             : JPH::EActivation::DontActivate);
    return BodyId{jid.GetIndexAndSequenceNumber()};
}

}  // namespace

BodyId PhysicsWorld::createStaticBox(Vec3 pos, Vec3 halfExtents) {
    JPH::ShapeRefC shape = new JPH::BoxShape(toJ(halfExtents));
    return createBodyImpl(*impl_, shape, toJ(pos),
                          JPH::EMotionType::Static, Layers::NON_MOVING, 0.0f);
}

BodyId PhysicsWorld::createDynamicBox(Vec3 pos, Vec3 halfExtents, float mass) {
    JPH::ShapeRefC shape = new JPH::BoxShape(toJ(halfExtents));
    return createBodyImpl(*impl_, shape, toJ(pos),
                          JPH::EMotionType::Dynamic, Layers::MOVING, mass);
}

BodyId PhysicsWorld::createDynamicSphere(Vec3 pos, float radius, float mass) {
    JPH::ShapeRefC shape = new JPH::SphereShape(radius);
    return createBodyImpl(*impl_, shape, toJ(pos),
                          JPH::EMotionType::Dynamic, Layers::MOVING, mass);
}

BodyId PhysicsWorld::createDynamicCapsule(Vec3 pos, float halfH, float r, float mass) {
    JPH::ShapeRefC shape = new JPH::CapsuleShape(halfH, r);
    return createBodyImpl(*impl_, shape, toJ(pos),
                          JPH::EMotionType::Dynamic, Layers::MOVING, mass);
}

void PhysicsWorld::destroyBody(BodyId b) {
    if (!b.isValid()) return;
    JPH::BodyInterface& bi = impl_->system->GetBodyInterface();
    bi.RemoveBody(toJoltBodyId(b));
    bi.DestroyBody(toJoltBodyId(b));
}

Vec3 PhysicsWorld::bodyPosition(BodyId b) const {
    if (!b.isValid()) return {};
    return toI(impl_->system->GetBodyInterface().GetCenterOfMassPosition(toJoltBodyId(b)));
}
Quat PhysicsWorld::bodyRotation(BodyId b) const {
    if (!b.isValid()) return {};
    return toI(impl_->system->GetBodyInterface().GetRotation(toJoltBodyId(b)));
}
Mat4 PhysicsWorld::bodyTransform(BodyId b) const {
    if (!b.isValid()) return Mat4::identity();
    const JPH::RVec3 p = impl_->system->GetBodyInterface().GetCenterOfMassPosition(toJoltBodyId(b));
    const JPH::Quat  q = impl_->system->GetBodyInterface().GetRotation(toJoltBodyId(b));
    return mat4FromJolt(p, q);
}
bool PhysicsWorld::isBodyAlive(BodyId b) const {
    if (!b.isValid()) return false;
    return impl_->system->GetBodyInterface().IsAdded(toJoltBodyId(b));
}

void PhysicsWorld::applyImpulse(BodyId b, Vec3 imp) {
    if (!b.isValid()) return;
    impl_->system->GetBodyInterface().AddImpulse(toJoltBodyId(b), toJ(imp));
}
void PhysicsWorld::applyForceAtPoint(BodyId b, Vec3 f, Vec3 worldP) {
    if (!b.isValid()) return;
    impl_->system->GetBodyInterface().AddForce(toJoltBodyId(b), toJ(f), toJ(worldP));
}
void PhysicsWorld::setVelocity(BodyId b, Vec3 v) {
    if (!b.isValid()) return;
    impl_->system->GetBodyInterface().SetLinearVelocity(toJoltBodyId(b), toJ(v));
}

PhysicsWorld::RaycastHit PhysicsWorld::raycast(Vec3 o, Vec3 d, float maxDist) const {
    RaycastHit out;
    if (!impl_->system) return out;
    JPH::RRayCast ray(toJ(o), toJ(d) * maxDist);
    JPH::RayCastResult hit;
    if (!impl_->system->GetNarrowPhaseQuery().CastRay(ray, hit)) {
        return out;
    }
    out.hit = true;
    out.body = BodyId{hit.mBodyID.GetIndexAndSequenceNumber()};
    out.t = hit.mFraction;
    const JPH::Vec3 pt = ray.GetPointOnRay(hit.mFraction);
    out.point = toI(pt);
    // Normal extraction requires body locking — placeholder for v1.
    out.normal = Vec3{0.0f, 1.0f, 0.0f};
    return out;
}

}  // namespace iron
