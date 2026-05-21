#include "test_framework.h"
#include "math/Quaternion.h"
#include "math/Vec.h"

#include <cmath>

using namespace iron;

int main() {
    constexpr float pi = 3.14159265358979323846f;

    // Identity quaternion rotates nothing.
    Quat id = Quat::identity();
    CHECK_NEAR(id.w, 1.0f);
    Vec3 p{1.0f, 0.0f, 0.0f};
    Vec3 same = id.rotate(p);
    CHECK_NEAR(same.x, 1.0f);

    // 90 degrees about Z turns +X into +Y.
    Quat rz = Quat::fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, pi / 2.0f);
    Vec3 r = rz.rotate(Vec3{1.0f, 0.0f, 0.0f});
    CHECK_NEAR(r.x, 0.0f);
    CHECK_NEAR(r.y, 1.0f);
    CHECK_NEAR(r.z, 0.0f);

    // Composing two 45-degree Z rotations equals one 90-degree rotation.
    Quat half = Quat::fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, pi / 4.0f);
    Quat full = half * half;
    Vec3 r2 = full.rotate(Vec3{1.0f, 0.0f, 0.0f});
    CHECK_NEAR(r2.x, 0.0f);
    CHECK_NEAR(r2.y, 1.0f);

    // A unit quaternion stays unit length after normalize.
    Quat n = rz.normalized();
    CHECK_NEAR(n.length(), 1.0f);

    // toMat4 agrees with rotate().
    Mat4 m = rz.toMat4();
    Vec4 mr = m * Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(mr.x, 0.0f);
    CHECK_NEAR(mr.y, 1.0f);

    return iron_test_result();
}
