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

    // rotationX(90 deg) turns +Y into +Z.
    Mat4 rx = rotationX(pi / 2.0f);
    Vec4 rxp = rx * Vec4{0.0f, 1.0f, 0.0f, 1.0f};
    CHECK_NEAR(rxp.y, 0.0f);
    CHECK_NEAR(rxp.z, 1.0f);

    // rotationY(90 deg) turns +Z into +X.
    Mat4 ry = rotationY(pi / 2.0f);
    Vec4 ryp = ry * Vec4{0.0f, 0.0f, 1.0f, 1.0f};
    CHECK_NEAR(ryp.x, 1.0f);
    CHECK_NEAR(ryp.z, 0.0f);

    // Composed model matrix T*R*S: applied right-to-left (scale, rotate,
    // translate). A unit +X point scaled x2, rotated 90 deg about Z, then
    // translated by (10,0,0) lands at (10, 2, 0).
    Mat4 model = translation(Vec3{10.0f, 0.0f, 0.0f}) *
                 rotationZ(pi / 2.0f) *
                 scaling(Vec3{2.0f, 2.0f, 2.0f});
    Vec4 mp = model * Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(mp.x, 10.0f);
    CHECK_NEAR(mp.y, 2.0f);
    CHECK_NEAR(mp.z, 0.0f);

    // lookAt: camera at +Z looking at origin keeps the origin in front (-Z).
    Mat4 view = lookAt(Vec3{0.0f, 0.0f, 5.0f},
                       Vec3{0.0f, 0.0f, 0.0f},
                       Vec3{0.0f, 1.0f, 0.0f});
    Vec4 originInView = view * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(originInView.z, -5.0f);

    // A world-space +X point stays on view-space +X (right/up rows correct).
    Vec4 xInView = view * Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(xInView.x, 1.0f);
    CHECK_NEAR(xInView.y, 0.0f);
    CHECK_NEAR(xInView.z, -5.0f);

    // perspective produces a finite, sensible matrix (corner entries set).
    Mat4 proj = perspective(pi / 4.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    CHECK(proj.at(3, 2) == -1.0f);
    CHECK(proj.at(0, 0) > 0.0f);
    CHECK(proj.at(1, 1) > 0.0f);

    // A point on the near plane maps to NDC z = -1, the far plane to z = +1
    // (after the perspective divide by w). View space looks down -Z.
    Vec4 nearClip = proj * Vec4{0.0f, 0.0f, -0.1f, 1.0f};
    CHECK_NEAR(nearClip.z / nearClip.w, -1.0f);
    Vec4 farClip = proj * Vec4{0.0f, 0.0f, -100.0f, 1.0f};
    CHECK_NEAR(farClip.z / farClip.w, 1.0f);

    // orthographic maps the box to NDC. Box: x,y in [-10,10], near=1,
    // far=100; view space looks down -Z.
    {
        Mat4 ortho = orthographic(-10.0f, 10.0f, -10.0f, 10.0f, 1.0f, 100.0f);
        // Centre of the box maps to the NDC origin in x and y.
        Vec4 centre = ortho * Vec4{0.0f, 0.0f, -50.5f, 1.0f};
        CHECK_NEAR(centre.x, 0.0f);
        CHECK_NEAR(centre.y, 0.0f);
        // Right/top edge maps to +1, +1.
        Vec4 corner = ortho * Vec4{10.0f, 10.0f, -1.0f, 1.0f};
        CHECK_NEAR(corner.x, 1.0f);
        CHECK_NEAR(corner.y, 1.0f);
        // The near plane maps to NDC z = -1, the far plane to z = +1.
        Vec4 nearP = ortho * Vec4{0.0f, 0.0f, -1.0f, 1.0f};
        CHECK_NEAR(nearP.z, -1.0f);
        Vec4 farP = ortho * Vec4{0.0f, 0.0f, -100.0f, 1.0f};
        CHECK_NEAR(farP.z, 1.0f);
        // Orthographic keeps w = 1 (no perspective divide).
        CHECK_NEAR(corner.w, 1.0f);
    }

    return iron_test_result();
}
