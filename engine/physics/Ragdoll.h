#pragma once

#include "physics/PhysicsWorld.h"

#include <array>

namespace iron {

struct RagdollSpec {
    float totalHeight = 1.8f;
    float mass        = 75.0f;
};

class Ragdoll {
public:
    static constexpr int kBoneCount  = 11;
    static constexpr int kJointCount = 10;

    static constexpr int kHead       = 0;
    static constexpr int kTorso      = 1;
    static constexpr int kHips       = 2;
    static constexpr int kUpperArmL  = 3;
    static constexpr int kForearmL   = 4;
    static constexpr int kUpperArmR  = 5;
    static constexpr int kForearmR   = 6;
    static constexpr int kUpperLegL  = 7;
    static constexpr int kLowerLegL  = 8;
    static constexpr int kUpperLegR  = 9;
    static constexpr int kLowerLegR  = 10;

    void spawn(PhysicsWorld& world, const RagdollSpec& spec,
               Vec3 position, Quat rotation = {0,0,0,1});
    void despawn(PhysicsWorld& world);

    int  boneCount() const { return kBoneCount; }
    Mat4 boneTransform(int idx)   const;
    Vec3 boneHalfExtents(int idx) const;
    Vec3 boneColor(int idx)       const;
    BodyId boneBody(int idx)      const;

    bool isSpawned() const { return bones_[kHips].isValid(); }

private:
    PhysicsWorld* world_ = nullptr;
    std::array<BodyId,  kBoneCount>  bones_      {};
    std::array<JointId, kJointCount> joints_     {};
    std::array<Vec3,    kBoneCount>  halfExtents_{};
};

}  // namespace iron
