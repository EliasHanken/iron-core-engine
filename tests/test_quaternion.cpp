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

    // 90 degrees about X turns +Y into +Z (exercises the x/y terms of the
    // math, which the Z-only tests above never touch).
    Quat rx = Quat::fromAxisAngle(Vec3{1.0f, 0.0f, 0.0f}, pi / 2.0f);
    Vec3 rxy = rx.rotate(Vec3{0.0f, 1.0f, 0.0f});
    CHECK_NEAR(rxy.x, 0.0f);
    CHECK_NEAR(rxy.y, 0.0f);
    CHECK_NEAR(rxy.z, 1.0f);

    // Composition is NOT commutative: Z-then-X differs from X-then-Z.
    Vec3 zThenX = (rx * rz).rotate(Vec3{0.0f, 1.0f, 0.0f});
    Vec3 xThenZ = (rz * rx).rotate(Vec3{0.0f, 1.0f, 0.0f});
    CHECK(std::fabs(zThenX.x - xThenZ.x) > 0.1f ||
          std::fabs(zThenX.y - xThenZ.y) > 0.1f ||
          std::fabs(zThenX.z - xThenZ.z) > 0.1f);

    // normalized() actually rescales a non-unit quaternion to unit length.
    Quat nonUnit{1.0f, 2.0f, 3.0f, 4.0f};
    CHECK_NEAR(nonUnit.normalized().length(), 1.0f);

    // toMat4 agrees with rotate(), checked on the X-axis case so the
    // off-diagonal matrix entries are genuinely exercised.
    Mat4 mx = rx.toMat4();
    Vec4 mxr = mx * Vec4{0.0f, 1.0f, 0.0f, 1.0f};
    CHECK_NEAR(mxr.x, 0.0f);
    CHECK_NEAR(mxr.y, 0.0f);
    CHECK_NEAR(mxr.z, 1.0f);

    return iron_test_result();
}
