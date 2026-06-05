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

    return iron_test_result();
}
