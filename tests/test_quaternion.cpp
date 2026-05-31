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

    // slerp at t=0 returns a, at t=1 returns b.
    {
        const Quat a = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.0f);
        const Quat b = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 1.5f);
        const Quat r0 = Quat::slerp(a, b, 0.0f);
        const Quat r1 = Quat::slerp(a, b, 1.0f);
        CHECK_NEAR(r0.x, a.x);
        CHECK_NEAR(r0.y, a.y);
        CHECK_NEAR(r0.z, a.z);
        CHECK_NEAR(r0.w, a.w);
        CHECK_NEAR(r1.x, b.x);
        CHECK_NEAR(r1.y, b.y);
        CHECK_NEAR(r1.z, b.z);
        CHECK_NEAR(r1.w, b.w);
    }

    // slerp halfway between two Y-axis rotations equals the half-angle rotation.
    {
        const Quat a = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.0f);
        const Quat b = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 1.0f);
        const Quat mid = Quat::slerp(a, b, 0.5f);
        const Quat expected = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.5f);
        CHECK_NEAR(mid.x, expected.x);
        CHECK_NEAR(mid.y, expected.y);
        CHECK_NEAR(mid.z, expected.z);
        CHECK_NEAR(mid.w, expected.w);
    }

    // slerp takes the shortest arc: a and -a are the same rotation, so the
    // halfway point along the short arc is a itself (up to sign).
    {
        const Quat a{0.0f, 0.7071f, 0.0f, 0.7071f};
        const Quat negA{-a.x, -a.y, -a.z, -a.w};
        const Quat mid = Quat::slerp(a, negA, 0.5f);
        CHECK_NEAR(std::fabs(mid.x), std::fabs(a.x));
        CHECK_NEAR(std::fabs(mid.w), std::fabs(a.w));
    }

    // iron::slerp free-function alias forwards to Quat::slerp identically.
    {
        const Quat a = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.0f);
        const Quat b = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 1.0f);
        const Quat viaStatic = Quat::slerp(a, b, 0.3f);
        const Quat viaAlias  = slerp(a, b, 0.3f);
        CHECK_NEAR(viaAlias.x, viaStatic.x);
        CHECK_NEAR(viaAlias.y, viaStatic.y);
        CHECK_NEAR(viaAlias.z, viaStatic.z);
        CHECK_NEAR(viaAlias.w, viaStatic.w);
    }

    // quatLookAt(eye, target, up) — identity case: eye at origin looking down -Z
    // with +Y up should produce a (near) identity quaternion.
    {
        const Quat q = quatLookAt(Vec3{0, 0, 0}, Vec3{0, 0, -1}, Vec3{0, 1, 0});
        const Vec3 fwd = q.rotate(Vec3{0, 0, -1});
        CHECK_NEAR(fwd.x, 0.0f);
        CHECK_NEAR(fwd.y, 0.0f);
        CHECK_NEAR(fwd.z, -1.0f);
    }

    // quatLookAt — looking from (+5, 0, 0) toward origin should produce a
    // rotation that maps the camera's local -Z onto the world direction (-1,0,0).
    {
        const Quat q = quatLookAt(Vec3{5, 0, 0}, Vec3{0, 0, 0}, Vec3{0, 1, 0});
        const Vec3 fwd = q.rotate(Vec3{0, 0, -1});
        CHECK_NEAR(fwd.x, -1.0f);
        CHECK_NEAR(fwd.y, 0.0f);
        CHECK_NEAR(fwd.z, 0.0f);
    }

    // quatLookAt — looking from (0, 5, 0) toward origin should map local -Z to
    // world (0, -1, 0). With +Y up colinear with the look direction we pass a
    // deliberate alternate up vector (+Z) — the test only checks the forward
    // axis, which is unambiguous.
    {
        const Quat q = quatLookAt(Vec3{0, 5, 0}, Vec3{0, 0, 0}, Vec3{0, 0, 1});
        const Vec3 fwd = q.rotate(Vec3{0, 0, -1});
        CHECK_NEAR(fwd.x, 0.0f);
        CHECK_NEAR(fwd.y, -1.0f);
        CHECK_NEAR(fwd.z, 0.0f);
    }

    return iron_test_result();
}
