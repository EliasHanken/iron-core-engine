#include "test_framework.h"
#include "math/Mat4.h"
#include "math/Vec.h"
#include "scene/FirstPersonController.h"

using namespace iron;

int main() {
    constexpr float pi = 3.14159265358979323846f;

    // Forward input at yaw 0 moves the player toward -Z.
    {
        FirstPersonController c;
        c.setPosition(Vec3{0.0f, 0.0f, 0.0f});
        c.setGroundHeight(0.0f);
        c.setMoveSpeed(10.0f);
        ControllerInput in;
        in.forward = 1.0f;
        c.update(in, 0.1f);  // 10 units/s * 0.1 s = 1 unit
        CHECK_NEAR(c.position().z, -1.0f);
        CHECK_NEAR(c.position().x, 0.0f);
    }

    // Strafe-right at yaw 0 moves the player toward +X.
    {
        FirstPersonController c;
        c.setPosition(Vec3{0.0f, 0.0f, 0.0f});
        c.setGroundHeight(0.0f);
        c.setMoveSpeed(10.0f);
        ControllerInput in;
        in.strafe = 1.0f;
        c.update(in, 0.1f);
        CHECK_NEAR(c.position().x, 1.0f);
        CHECK_NEAR(c.position().z, 0.0f);
    }

    // Mouse movement in X increases yaw.
    {
        FirstPersonController c;
        c.setMouseSensitivity(0.01f);
        ControllerInput in;
        in.mouseDX = 10.0f;
        c.update(in, 0.016f);
        CHECK_NEAR(c.yaw(), 0.1f);  // 10 px * 0.01 rad/px
    }

    // Pitch is clamped away from straight up.
    {
        FirstPersonController c;
        c.setMouseSensitivity(0.01f);
        ControllerInput in;
        in.mouseDY = -100000.0f;  // a huge upward look
        c.update(in, 0.016f);
        CHECK(c.pitch() < pi / 2.0f);  // never reaches straight up
        CHECK(c.pitch() > 1.5f);       // but does clamp close to it
    }

    // Gravity pulls the player down and the ground clamp holds them there.
    {
        FirstPersonController c;
        c.setGroundHeight(0.0f);
        c.setPosition(Vec3{0.0f, 5.0f, 0.0f});
        ControllerInput in;  // no movement input
        for (int i = 0; i < 600; ++i) {
            c.update(in, 1.0f / 60.0f);  // ~10 seconds of falling
        }
        CHECK_NEAR(c.position().y, 0.0f);  // resting exactly on the ground
    }

    // The player never sinks below the ground in a single step.
    {
        FirstPersonController c;
        c.setGroundHeight(2.0f);
        c.setPosition(Vec3{0.0f, 2.0f, 0.0f});
        ControllerInput in;
        c.update(in, 1.0f / 60.0f);
        CHECK(c.position().y >= 2.0f);
    }

    // eyePosition is the feet position plus the eye height.
    {
        FirstPersonController c;
        c.setPosition(Vec3{1.0f, 0.0f, 3.0f});
        c.setEyeHeight(1.7f);
        Vec3 eye = c.eyePosition();
        CHECK_NEAR(eye.x, 1.0f);
        CHECK_NEAR(eye.y, 1.7f);
        CHECK_NEAR(eye.z, 3.0f);
    }

    // viewMatrix: at the origin looking toward -Z, a world point at -Z is in
    // front of the camera (negative view-space z) and stays centred.
    {
        FirstPersonController c;
        c.setPosition(Vec3{0.0f, 0.0f, 0.0f});
        c.setEyeHeight(0.0f);
        Mat4 v = c.viewMatrix();
        Vec4 p = v * Vec4{0.0f, 0.0f, -5.0f, 1.0f};
        CHECK(p.z < 0.0f);
        CHECK_NEAR(p.x, 0.0f);
        CHECK_NEAR(p.y, 0.0f);
    }

    // Pitch is clamped away from straight down (the lower clamp).
    {
        FirstPersonController c;
        c.setMouseSensitivity(0.01f);
        ControllerInput in;
        in.mouseDY = 100000.0f;  // a huge downward look
        c.update(in, 0.016f);
        CHECK(c.pitch() > -pi / 2.0f);  // never reaches straight down
        CHECK(c.pitch() < -1.5f);       // but does clamp close to it
    }

    // After rotating yaw 90 degrees, "forward" moves along the new facing.
    {
        FirstPersonController c;
        c.setPosition(Vec3{0.0f, 0.0f, 0.0f});
        c.setGroundHeight(0.0f);
        c.setMoveSpeed(10.0f);
        c.setMouseSensitivity(1.0f);
        ControllerInput yawInput;
        yawInput.mouseDX = pi / 2.0f;  // rotate yaw by 90 degrees
        c.update(yawInput, 0.0f);      // dt = 0: only the rotation applies
        ControllerInput moveInput;
        moveInput.forward = 1.0f;
        c.update(moveInput, 0.1f);
        CHECK_NEAR(c.position().x, -1.0f);  // forward now points along -X
        CHECK_NEAR(c.position().z, 0.0f);
    }

    // Diagonal input (forward + strafe) is normalized — not sqrt(2) faster.
    {
        FirstPersonController c;
        c.setPosition(Vec3{0.0f, 0.0f, 0.0f});
        c.setGroundHeight(0.0f);
        c.setMoveSpeed(10.0f);
        ControllerInput in;
        in.forward = 1.0f;
        in.strafe = 1.0f;
        c.update(in, 0.1f);
        const float dx = c.position().x;
        const float dz = c.position().z;
        CHECK_NEAR(std::sqrt(dx * dx + dz * dz), 1.0f);
    }

    return iron_test_result();
}
