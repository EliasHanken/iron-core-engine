#pragma once

#include "asset/Animation.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Vec.h"

#include <span>
#include <vector>

namespace iron {

// One bone's LOCAL transform, decomposed so it can be blended before the
// hierarchy is applied. Defaults are the identity transform.
struct BoneLocal {
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Quat rotation = Quat::identity();
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// A full local-space pose: one BoneLocal per skeleton bone, in bone order.
struct Pose {
    std::vector<BoneLocal> bones;
};

// Column-major TRS compose (M = T * R * S, no shear).
Mat4 composeTRS(Vec3 t, Quat r, Vec3 s);
Mat4 composeTRS(const BoneLocal& b);

// Decompose a column-major TRS Mat4 (no shear) into T/R/S. Scale comes from
// basis-column lengths; rotation from the normalized basis as a quaternion.
BoneLocal decomposeTRS(const Mat4& m);

// Fill `out` with the skeleton's bind pose (each localBindTransform decomposed).
void bindPose(const Skeleton& skeleton, Pose& out);

// Sample a clip at `time` into a local Pose: seed from the bind pose, then let
// each channel overwrite the component it drives. `out` is resized to the
// skeleton's bone count.
void samplePose(const Skeleton& skeleton, const AnimationClip& clip,
                float time, Pose& out);

// Compose a local Pose up the hierarchy into global bone transforms. Writes
// min(out, pose, skeleton) bones; parents must precede children (loader
// guarantees this).
void composeGlobals(const Skeleton& skeleton, const Pose& pose,
                    std::span<Mat4> outGlobals);

// Write the skinning palette (global * inverseBind) from precomputed globals.
void globalsToPalette(const Skeleton& skeleton, std::span<const Mat4> globals,
                      std::span<Mat4> out);

// Convenience: composeGlobals + globalsToPalette (no IK pass).
void posePalette(const Skeleton& skeleton, const Pose& pose,
                 std::span<Mat4> out);

}  // namespace iron
