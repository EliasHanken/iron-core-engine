#include "test_framework.h"
#include "math/Mat4.h"
#include "math/Vec.h"
#include "render/ReflectionPlane.h"
#include "render/Renderer.h"  // for DrawCall

using namespace iron;

int main() {
    // 1. Reflection across y = 0: (1, 2, 3) -> (1, -2, 3).
    {
        ReflectionPlane plane;
        plane.normal = Vec3{0.0f, 1.0f, 0.0f};
        plane.d = 0.0f;
        Mat4 M = reflectionMatrix(plane);
        Vec4 p = M * Vec4{1.0f, 2.0f, 3.0f, 1.0f};
        CHECK_NEAR(p.x, 1.0f);
        CHECK_NEAR(p.y, -2.0f);
        CHECK_NEAR(p.z, 3.0f);
        CHECK_NEAR(p.w, 1.0f);
    }

    // 2. Reflection across y = -3 (normal {0,1,0}, d = -3):
    //    (0, 1, 0) is 4 units above the plane -> reflects to (0, -7, 0).
    {
        ReflectionPlane plane;
        plane.normal = Vec3{0.0f, 1.0f, 0.0f};
        plane.d = -3.0f;
        Mat4 M = reflectionMatrix(plane);
        Vec4 p = M * Vec4{0.0f, 1.0f, 0.0f, 1.0f};
        CHECK_NEAR(p.x, 0.0f);
        CHECK_NEAR(p.y, -7.0f);
        CHECK_NEAR(p.z, 0.0f);
    }

    // 3. The matrix is its own inverse: M * M = I.
    {
        ReflectionPlane plane;
        plane.normal = Vec3{0.0f, 1.0f, 0.0f};
        plane.d = -3.0f;
        Mat4 M = reflectionMatrix(plane);
        Mat4 MM = M * M;
        Vec4 p = MM * Vec4{5.0f, 7.0f, 9.0f, 1.0f};
        CHECK_NEAR(p.x, 5.0f);
        CHECK_NEAR(p.y, 7.0f);
        CHECK_NEAR(p.z, 9.0f);
    }

    // 4. A point on the plane reflects to itself.
    //    For normal {0,1,0}, d = -3, the plane is y = -3.
    {
        ReflectionPlane plane;
        plane.normal = Vec3{0.0f, 1.0f, 0.0f};
        plane.d = -3.0f;
        Mat4 M = reflectionMatrix(plane);
        Vec4 p = M * Vec4{2.0f, -3.0f, 4.0f, 1.0f};
        CHECK_NEAR(p.x, 2.0f);
        CHECK_NEAR(p.y, -3.0f);
        CHECK_NEAR(p.z, 4.0f);
    }

    // 5. A point above and its reflection are equidistant from the plane.
    {
        ReflectionPlane plane;
        plane.normal = Vec3{0.0f, 1.0f, 0.0f};
        plane.d = -3.0f;
        Mat4 M = reflectionMatrix(plane);
        Vec4 above = Vec4{0.0f, 5.0f, 0.0f, 1.0f};        // 8 above y=-3
        Vec4 below = M * above;
        CHECK_NEAR(above.y - (-3.0f), 8.0f);
        CHECK_NEAR((-3.0f) - below.y, 8.0f);
    }

    // 6. DrawCall reflection defaults.
    {
        DrawCall d;
        CHECK_NEAR(d.reflectivity, 0.0f);
        CHECK(d.useReflectionPlane == false);
    }

    return iron_test_result();
}
