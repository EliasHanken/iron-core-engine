#include "test_framework.h"
#include "math/Mat4.h"
#include "math/Vec.h"
#include "scene/FreeFlyCamera.h"

using namespace iron;

int main() {
    constexpr float pi = 3.14159265358979323846f;

    // At yaw=0, pitch=0 the forward vector is -Z.
    {
        FreeFlyCamera c;
        c.yaw = 0.0f;
        c.pitch = 0.0f;
        const Vec3 f = c.forward();
        CHECK_NEAR(f.x, 0.0f);
        CHECK_NEAR(f.y, 0.0f);
        CHECK_NEAR(f.z, -1.0f);
    }

    // Forward input (W) at yaw=0 moves position toward -Z.
    {
        FreeFlyCamera c;
        c.position = Vec3{0.0f, 0.0f, 0.0f};
        c.update(0.1f, 0.0f, 0.0f,
                 /*fwd*/true, /*back*/false, /*left*/false, /*right*/false,
                 /*worldDown*/false, /*worldUp*/false,
                 /*moveSpeed*/10.0f);
        // 10 units/s * 0.1 s = 1 unit toward -Z
        CHECK_NEAR(c.position.z, -1.0f);
        CHECK_NEAR(c.position.x, 0.0f);
        CHECK_NEAR(c.position.y, 0.0f);
    }

    // Strafe-right (D) at yaw=0 moves position toward +X.
    {
        FreeFlyCamera c;
        c.position = Vec3{0.0f, 0.0f, 0.0f};
        c.update(0.1f, 0.0f, 0.0f,
                 false, false, false, /*right*/true,
                 false, false,
                 10.0f);
        CHECK_NEAR(c.position.x, 1.0f);
        CHECK_NEAR(c.position.z, 0.0f);
    }

    // World-up (E) moves +Y regardless of pitch.
    {
        FreeFlyCamera c;
        c.position = Vec3{0.0f, 0.0f, 0.0f};
        c.pitch = 0.5f;  // pitched up — should not affect Q/E
        c.update(0.1f, 0.0f, 0.0f,
                 false, false, false, false,
                 /*worldDown*/false, /*worldUp*/true,
                 10.0f);
        CHECK_NEAR(c.position.y, 1.0f);
        CHECK_NEAR(c.position.x, 0.0f);
        CHECK_NEAR(c.position.z, 0.0f);
    }

    // Mouse-right (positive mouseDx) turns the view right (yaw decreases).
    {
        FreeFlyCamera c;
        c.yaw = 0.0f;
        c.update(0.016f, /*mouseDx*/10.0f, 0.0f,
                 false, false, false, false, false, false,
                 5.0f, /*mouseSensitivity*/0.01f);
        // 10 px * 0.01 rad/px, turning right -> yaw -= 0.1
        CHECK_NEAR(c.yaw, -0.1f);
    }

    // Pitch is clamped just under +pi/2.
    {
        FreeFlyCamera c;
        c.pitch = 0.0f;
        // Push pitch hard upward: many ticks of big mouseDy
        for (int i = 0; i < 100; ++i) {
            c.update(0.016f, 0.0f, /*mouseDy*/-100.0f,
                     false, false, false, false, false, false,
                     5.0f, /*mouseSensitivity*/0.01f);
        }
        // Clamp is +/- 89 degrees = 1.5533 rad
        CHECK(c.pitch < pi * 0.5f);
        CHECK(c.pitch > pi * 0.49f);
    }

    // viewMatrix transforms a world point in front of the camera onto -Z in view space.
    {
        FreeFlyCamera c;
        c.position = Vec3{0.0f, 0.0f, 5.0f};
        c.yaw = 0.0f;
        c.pitch = 0.0f;
        // Point 1 unit in front of the camera (toward -Z in world): {0, 0, 4}.
        const Vec3 inFront{0.0f, 0.0f, 4.0f};
        const Mat4 v = c.viewMatrix();
        // Mat4 has operator*(Mat4, Vec4) — transform as homogeneous point (w=1).
        const Vec4 eyeH = v * Vec4{inFront.x, inFront.y, inFront.z, 1.0f};
        const Vec3 eye{eyeH.x, eyeH.y, eyeH.z};
        // In view space the point should be at (0, 0, -1).
        CHECK_NEAR(eye.x, 0.0f);
        CHECK_NEAR(eye.y, 0.0f);
        CHECK_NEAR(eye.z, -1.0f);
    }

    return iron_test_result();
}
