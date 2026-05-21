#include "test_framework.h"
#include "math/Mat4.h"
#include "math/Transform.h"
#include "math/Vec.h"

using namespace iron;

int main() {
    constexpr float pi = 3.14159265358979323846f;

    // translate moves a point.
    Mat4 t = translation(Vec3{1.0f, 2.0f, 3.0f});
    Vec4 p = t * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(p.x, 1.0f);
    CHECK_NEAR(p.y, 2.0f);
    CHECK_NEAR(p.z, 3.0f);

    // A direction (w = 0) is NOT moved by a translation.
    Vec4 dir = t * Vec4{1.0f, 0.0f, 0.0f, 0.0f};
    CHECK_NEAR(dir.x, 1.0f);

    // scale stretches a point.
    Mat4 s = scaling(Vec3{2.0f, 3.0f, 4.0f});
    Vec4 sp = s * Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    CHECK_NEAR(sp.x, 2.0f);
    CHECK_NEAR(sp.y, 3.0f);
    CHECK_NEAR(sp.z, 4.0f);

    // rotationZ(90 deg) turns +X into +Y.
    Mat4 r = rotationZ(pi / 2.0f);
    Vec4 rp = r * Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(rp.x, 0.0f);
    CHECK_NEAR(rp.y, 1.0f);

    // lookAt: camera at +Z looking at origin keeps the origin in front (-Z).
    Mat4 view = lookAt(Vec3{0.0f, 0.0f, 5.0f},
                       Vec3{0.0f, 0.0f, 0.0f},
                       Vec3{0.0f, 1.0f, 0.0f});
    Vec4 originInView = view * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(originInView.z, -5.0f);

    // perspective produces a finite, sensible matrix (corner entries set).
    Mat4 proj = perspective(pi / 4.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    CHECK(proj.at(3, 2) == -1.0f);
    CHECK(proj.at(0, 0) > 0.0f);
    CHECK(proj.at(1, 1) > 0.0f);

    return iron_test_result();
}
