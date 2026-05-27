// Ragdoll.cpp — 11-body humanoid skeleton + 10 joints.

#include "physics/Ragdoll.h"

#include <cmath>
#include <numbers>

namespace iron {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDeg = kPi / 180.0f;

struct BoneSpec {
    Vec3  halfExtentsFracHeight;
    float massFrac;
    Vec3  colorRgb;
};

constexpr BoneSpec kBoneSpecs[Ragdoll::kBoneCount] = {
    {{0.056f, 0.069f, 0.056f}, 0.08f,  {1.0f, 0.40f, 0.40f}},  // head — red
    {{0.100f, 0.139f, 0.061f}, 0.36f,  {0.40f, 0.60f, 1.0f}},  // torso — blue
    {{0.100f, 0.050f, 0.061f}, 0.14f,  {0.30f, 0.45f, 0.80f}}, // hips
    {{0.028f, 0.083f, 0.028f}, 0.03f,  {0.40f, 1.00f, 0.40f}}, // upper arm L
    {{0.022f, 0.089f, 0.022f}, 0.025f, {0.30f, 0.85f, 0.30f}}, // forearm L
    {{0.028f, 0.083f, 0.028f}, 0.03f,  {0.40f, 1.00f, 0.40f}}, // upper arm R
    {{0.022f, 0.089f, 0.022f}, 0.025f, {0.30f, 0.85f, 0.30f}}, // forearm R
    {{0.036f, 0.117f, 0.036f}, 0.09f,  {1.00f, 0.85f, 0.30f}}, // upper leg L
    {{0.028f, 0.117f, 0.028f}, 0.06f,  {0.85f, 0.70f, 0.20f}}, // lower leg L
    {{0.036f, 0.117f, 0.036f}, 0.09f,  {1.00f, 0.85f, 0.30f}}, // upper leg R
    {{0.028f, 0.117f, 0.028f}, 0.06f,  {0.85f, 0.70f, 0.20f}}, // lower leg R
};

constexpr float kY[Ragdoll::kBoneCount] = {
    0.443f, 0.250f, 0.000f, 0.225f, 0.067f, 0.225f, 0.067f, -0.167f, -0.400f, -0.167f, -0.400f,
};
constexpr float kX[Ragdoll::kBoneCount] = {
    0.0f, 0.0f, 0.0f, -0.131f, -0.131f, +0.131f, +0.131f, -0.061f, -0.061f, +0.061f, +0.061f,
};

}  // namespace

void Ragdoll::spawn(PhysicsWorld& world, const RagdollSpec& spec,
                    Vec3 position, Quat /*rotation*/) {
    world_ = &world;
    const float H = spec.totalHeight;

    for (int i = 0; i < kBoneCount; ++i) {
        const Vec3 he = {
            kBoneSpecs[i].halfExtentsFracHeight.x * H,
            kBoneSpecs[i].halfExtentsFracHeight.y * H,
            kBoneSpecs[i].halfExtentsFracHeight.z * H,
        };
        halfExtents_[i] = he;
        const Vec3 pos = {
            position.x + kX[i] * H,
            position.y + kY[i] * H,
            position.z,
        };
        bones_[i] = world.createDynamicBox(pos, he, spec.mass * kBoneSpecs[i].massFrac);
    }

    auto pivotBetween = [&](int a, int b) -> Vec3 {
        return Vec3{
            position.x + ((kX[a] + kX[b]) * 0.5f) * H,
            position.y + ((kY[a] + kY[b]) * 0.5f) * H,
            position.z,
        };
    };

    int j = 0;
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kTorso], bones_[kHips], pivotBetween(kTorso, kHips),
        Vec3{0,1,0}, 25.0f * kDeg, 15.0f * kDeg);
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kHead], bones_[kTorso], pivotBetween(kHead, kTorso),
        Vec3{0,1,0}, 40.0f * kDeg, 60.0f * kDeg);
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kUpperArmL], bones_[kTorso], pivotBetween(kUpperArmL, kTorso),
        Vec3{0,1,0}, 90.0f * kDeg, 45.0f * kDeg);
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kUpperArmR], bones_[kTorso], pivotBetween(kUpperArmR, kTorso),
        Vec3{0,1,0}, 90.0f * kDeg, 45.0f * kDeg);
    joints_[j++] = world.createHingeJoint(
        bones_[kForearmL], bones_[kUpperArmL], pivotBetween(kForearmL, kUpperArmL),
        Vec3{1,0,0}, 0.0f, 145.0f * kDeg);
    joints_[j++] = world.createHingeJoint(
        bones_[kForearmR], bones_[kUpperArmR], pivotBetween(kForearmR, kUpperArmR),
        Vec3{1,0,0}, 0.0f, 145.0f * kDeg);
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kUpperLegL], bones_[kHips], pivotBetween(kUpperLegL, kHips),
        Vec3{0,1,0}, 60.0f * kDeg, 30.0f * kDeg);
    joints_[j++] = world.createSwingTwistJoint(
        bones_[kUpperLegR], bones_[kHips], pivotBetween(kUpperLegR, kHips),
        Vec3{0,1,0}, 60.0f * kDeg, 30.0f * kDeg);
    joints_[j++] = world.createHingeJoint(
        bones_[kLowerLegL], bones_[kUpperLegL], pivotBetween(kLowerLegL, kUpperLegL),
        Vec3{1,0,0}, -145.0f * kDeg, 0.0f);
    joints_[j++] = world.createHingeJoint(
        bones_[kLowerLegR], bones_[kUpperLegR], pivotBetween(kLowerLegR, kUpperLegR),
        Vec3{1,0,0}, -145.0f * kDeg, 0.0f);
}

void Ragdoll::despawn(PhysicsWorld& world) {
    for (auto& j : joints_) {
        if (j.isValid()) {
            world.destroyJoint(j);
            j = kInvalidJoint;
        }
    }
    for (auto& b : bones_) {
        if (b.isValid()) {
            world.destroyBody(b);
            b = kInvalidBody;
        }
    }
    world_ = nullptr;
}

Mat4 Ragdoll::boneTransform(int idx) const {
    if (idx < 0 || idx >= kBoneCount || !world_) return Mat4::identity();
    return world_->bodyTransform(bones_[idx]);
}

Vec3 Ragdoll::boneHalfExtents(int idx) const {
    if (idx < 0 || idx >= kBoneCount) return {};
    return halfExtents_[idx];
}

Vec3 Ragdoll::boneColor(int idx) const {
    if (idx < 0 || idx >= kBoneCount) return {1,1,1};
    return kBoneSpecs[idx].colorRgb;
}

BodyId Ragdoll::boneBody(int idx) const {
    if (idx < 0 || idx >= kBoneCount) return kInvalidBody;
    return bones_[idx];
}

}  // namespace iron
