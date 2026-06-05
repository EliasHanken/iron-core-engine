#include "asset/Animation.h"
#include "asset/Pose.h"
#include "asset/Skeleton.h"
#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <array>

using namespace iron;

int main() {
    // decompose(compose(t,r,s)) round-trips.
    {
        const Vec3 t{3.0f, -2.0f, 5.0f};
        const Quat r = Quat::fromAxisAngle(Vec3{0, 1, 0}, 1.0f);
        const Vec3 s{2.0f, 2.0f, 2.0f};
        const BoneLocal b = decomposeTRS(composeTRS(t, r, s));
        CHECK_NEAR(b.translation.x, 3.0f);
        CHECK_NEAR(b.translation.y, -2.0f);
        CHECK_NEAR(b.translation.z, 5.0f);
        CHECK_NEAR(b.scale.x, 2.0f);
        CHECK_NEAR(b.scale.y, 2.0f);
        CHECK_NEAR(b.scale.z, 2.0f);
        // Rotation sign can flip (q == -q); compare via the rotated basis.
        const Vec3 v = b.rotation.rotate(Vec3{1, 0, 0});
        const Vec3 w = r.rotate(Vec3{1, 0, 0});
        CHECK_NEAR(v.x, w.x);
        CHECK_NEAR(v.y, w.y);
        CHECK_NEAR(v.z, w.z);
    }

    // bindPose decomposes each bone's localBindTransform.
    {
        Skeleton sk;
        Bone root;
        root.parentIndex = -1;
        root.localBindTransform = composeTRS(Vec3{1, 2, 3}, Quat::identity(),
                                             Vec3{1, 1, 1});
        root.inverseBindMatrix = Mat4::identity();
        sk.bones.push_back(root);

        Pose p;
        bindPose(sk, p);
        CHECK(p.bones.size() == 1);
        CHECK_NEAR(p.bones[0].translation.x, 1.0f);
        CHECK_NEAR(p.bones[0].translation.y, 2.0f);
        CHECK_NEAR(p.bones[0].translation.z, 3.0f);
    }

    // samplePose preserves the bind-pose seed for an empty (failed-load)
    // sampler instead of overwriting translation/scale with zeros.
    {
        Skeleton sk;
        Bone root;
        root.parentIndex = -1;
        root.localBindTransform = composeTRS(Vec3{4, 5, 6}, Quat::identity(),
                                             Vec3{1, 1, 1});
        root.inverseBindMatrix = Mat4::identity();
        sk.bones.push_back(root);

        AnimationClip clip;
        clip.samplers.emplace_back();  // empty sampler (no keyframes)
        AnimationChannel ch;
        ch.targetBone = 0;
        ch.path = AnimationPath::Translation;
        ch.samplerIndex = 0;
        clip.channels.push_back(ch);

        Pose p;
        samplePose(sk, clip, 0.0f, p);
        CHECK(p.bones.size() == 1);
        CHECK_NEAR(p.bones[0].translation.x, 4.0f);
        CHECK_NEAR(p.bones[0].translation.y, 5.0f);
        CHECK_NEAR(p.bones[0].translation.z, 6.0f);
    }

    // samplePose drives a bone's translation from a clip, then posePalette
    // produces global * inverseBind (identity inverseBind => global).
    {
        Skeleton sk;
        Bone root;
        root.parentIndex = -1;
        root.localBindTransform = Mat4::identity();
        root.inverseBindMatrix = Mat4::identity();
        sk.bones.push_back(root);

        AnimationClip clip;
        clip.name = "slide";
        clip.duration = 1.0f;
        AnimationSampler s;
        s.inputs = {0.0f, 1.0f};
        s.outputs = {0, 0, 0,  10, 0, 0};
        s.interpolation = AnimationInterpolation::Linear;
        clip.samplers.push_back(s);
        AnimationChannel ch;
        ch.targetBone = 0;
        ch.path = AnimationPath::Translation;
        ch.samplerIndex = 0;
        clip.channels.push_back(ch);

        Pose pose;
        samplePose(sk, clip, 0.5f, pose);
        CHECK_NEAR(pose.bones[0].translation.x, 5.0f);

        std::array<Mat4, 1> palette{};
        posePalette(sk, pose, palette);
        CHECK_NEAR(palette[0].at(0, 3), 5.0f);
    }

    // composeGlobals concatenates a parent translation into a child.
    {
        Skeleton sk;
        Bone root;
        root.parentIndex = -1;
        root.localBindTransform = composeTRS(Vec3{10, 0, 0}, Quat::identity(), Vec3{1, 1, 1});
        root.inverseBindMatrix = Mat4::identity();
        Bone child;
        child.parentIndex = 0;
        child.localBindTransform = composeTRS(Vec3{1, 0, 0}, Quat::identity(), Vec3{1, 1, 1});
        child.inverseBindMatrix = Mat4::identity();
        sk.bones.push_back(root);
        sk.bones.push_back(child);

        Pose pose;
        bindPose(sk, pose);
        std::array<Mat4, 2> globals{};
        composeGlobals(sk, pose, globals);
        CHECK_NEAR(globals[0].at(0, 3), 10.0f);
        CHECK_NEAR(globals[1].at(0, 3), 11.0f);  // 10 (parent) + 1 (child)
    }

    return iron_test_result();
}
