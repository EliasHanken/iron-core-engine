#include "editor/ViewGizmo.h"
#include "math/Vec.h"
#include "scene/FreeFlyCamera.h"
#include "test_framework.h"

#include <cmath>

using namespace iron;

int main() {
    constexpr float pi = 3.14159265358979323846f;
    constexpr float kIsoYaw   = pi / 4.0f;            // +45° (positive)
    const     float kIsoPitch = -std::asin(1.0f / std::sqrt(3.0f));  // ≈ -35.26°

    // --- Test 1: default distance positions camera at (10, 10, 10) ---
    {
        FreeFlyCamera cam;
        setIsometricView(cam);
        CHECK_NEAR(cam.position.x, 10.0f);
        CHECK_NEAR(cam.position.y, 10.0f);
        CHECK_NEAR(cam.position.z, 10.0f);
    }

    // --- Test 2: yaw / pitch match the diagonal direction ---
    {
        FreeFlyCamera cam;
        setIsometricView(cam);
        CHECK_NEAR(cam.yaw,   kIsoYaw);
        CHECK_NEAR(cam.pitch, kIsoPitch);
    }

    // --- Test 3: forward direction points toward origin from (+,+,+) ---
    {
        FreeFlyCamera cam;
        setIsometricView(cam);
        const Vec3 fwd = cam.forward();
        const Vec3 expected = normalize(Vec3{-1.0f, -1.0f, -1.0f});
        CHECK_NEAR(fwd.x, expected.x);
        CHECK_NEAR(fwd.y, expected.y);
        CHECK_NEAR(fwd.z, expected.z);
    }

    // --- Test 4: distance parameter scales position linearly ---
    {
        FreeFlyCamera cam;
        setIsometricView(cam, Vec3{0.0f, 0.0f, 0.0f}, 25.0f);
        CHECK_NEAR(cam.position.x, 25.0f);
        CHECK_NEAR(cam.position.y, 25.0f);
        CHECK_NEAR(cam.position.z, 25.0f);
        CHECK_NEAR(cam.yaw,   kIsoYaw);
        CHECK_NEAR(cam.pitch, kIsoPitch);
    }

    // --- Test 5: call is idempotent ---
    {
        FreeFlyCamera cam;
        setIsometricView(cam, Vec3{0.0f, 0.0f, 0.0f}, 10.0f);
        setIsometricView(cam, Vec3{0.0f, 0.0f, 0.0f}, 10.0f);
        CHECK_NEAR(cam.yaw,   kIsoYaw);
        CHECK_NEAR(cam.pitch, kIsoPitch);
    }

    // --- Test 6: pivot translates the camera position (orientation unchanged) ---
    {
        FreeFlyCamera cam;
        const Vec3 pivot{5.0f, -3.0f, 2.0f};
        setIsometricView(cam, pivot, 10.0f);
        CHECK_NEAR(cam.position.x, 15.0f);   // 5 + 10
        CHECK_NEAR(cam.position.y, 7.0f);    // -3 + 10
        CHECK_NEAR(cam.position.z, 12.0f);   // 2 + 10
        // Forward direction (and therefore yaw/pitch) unchanged from origin case.
        CHECK_NEAR(cam.yaw,   kIsoYaw);
        CHECK_NEAR(cam.pitch, kIsoPitch);
    }

    return iron_test_result();
}
