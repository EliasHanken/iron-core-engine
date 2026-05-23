#include "test_framework.h"
#include "math/Vec.h"
#include "render/Light.h"
#include "render/PointLightMath.h"

using namespace iron;

int main() {
    // 1. PointLight defaults match the spec.
    PointLight defaults;
    CHECK_NEAR(defaults.position.x, 0.0f);
    CHECK_NEAR(defaults.position.y, 0.0f);
    CHECK_NEAR(defaults.position.z, 0.0f);
    CHECK_NEAR(defaults.color.x, 1.0f);
    CHECK_NEAR(defaults.color.y, 1.0f);
    CHECK_NEAR(defaults.color.z, 1.0f);
    CHECK_NEAR(defaults.intensity, 1.0f);
    CHECK_NEAR(defaults.range, 5.0f);

    // 2. Distance = 0 (degenerate): contribution is exactly zero.
    {
        PointLight light;
        light.position = Vec3{0.0f, 0.0f, 0.0f};
        light.color = Vec3{1.0f, 1.0f, 1.0f};
        light.intensity = 1.0f;
        light.range = 5.0f;
        Vec3 fragPos{0.0f, 0.0f, 0.0f};
        Vec3 normal{0.0f, 1.0f, 0.0f};
        Vec3 c = pointLightContribution(light, fragPos, normal);
        CHECK_NEAR(c.x, 0.0f);
        CHECK_NEAR(c.y, 0.0f);
        CHECK_NEAR(c.z, 0.0f);
    }

    // 3. Distance >= range: contribution is zero (the cull).
    {
        PointLight light;
        light.position = Vec3{10.0f, 0.0f, 0.0f}; // 10 units away
        light.range = 5.0f;                       // < distance
        light.intensity = 1.0f;
        light.color = Vec3{1.0f, 1.0f, 1.0f};
        Vec3 fragPos{0.0f, 0.0f, 0.0f};
        Vec3 normal{1.0f, 0.0f, 0.0f}; // facing the light
        Vec3 c = pointLightContribution(light, fragPos, normal);
        CHECK_NEAR(c.x, 0.0f);
        CHECK_NEAR(c.y, 0.0f);
        CHECK_NEAR(c.z, 0.0f);
    }

    // 4. Distance = range/2: falloff is exactly 0.5.
    // Math: t = 2.5/5 = 0.5, smoothstep = 0.5^2*(3 - 2*0.5) = 0.25*2 = 0.5,
    // falloff = 1 - 0.5 = 0.5. lambert = 1 (normal perfectly aligned), intensity = 1.
    {
        PointLight light;
        light.position = Vec3{2.5f, 0.0f, 0.0f}; // half of range 5
        light.range = 5.0f;
        light.intensity = 1.0f;
        light.color = Vec3{1.0f, 1.0f, 1.0f};
        Vec3 fragPos{0.0f, 0.0f, 0.0f};
        Vec3 normal{1.0f, 0.0f, 0.0f}; // facing the light
        Vec3 c = pointLightContribution(light, fragPos, normal);
        CHECK_NEAR(c.x, 0.5f);
        CHECK_NEAR(c.y, 0.5f);
        CHECK_NEAR(c.z, 0.5f);
    }

    // 5. Normal facing away from the light: contribution is zero.
    {
        PointLight light;
        light.position = Vec3{2.0f, 0.0f, 0.0f}; // inside range
        light.range = 5.0f;
        light.intensity = 1.0f;
        light.color = Vec3{1.0f, 1.0f, 1.0f};
        Vec3 fragPos{0.0f, 0.0f, 0.0f};
        Vec3 normal{-1.0f, 0.0f, 0.0f}; // facing AWAY from the light
        Vec3 c = pointLightContribution(light, fragPos, normal);
        CHECK_NEAR(c.x, 0.0f);
        CHECK_NEAR(c.y, 0.0f);
        CHECK_NEAR(c.z, 0.0f);
    }

    // 6. Intensity scales the result: intensity=2 at half-range gives 1.0
    // (double of test 4's 0.5).
    {
        PointLight light;
        light.position = Vec3{2.5f, 0.0f, 0.0f};
        light.range = 5.0f;
        light.intensity = 2.0f;
        light.color = Vec3{1.0f, 1.0f, 1.0f};
        Vec3 fragPos{0.0f, 0.0f, 0.0f};
        Vec3 normal{1.0f, 0.0f, 0.0f};
        Vec3 c = pointLightContribution(light, fragPos, normal);
        CHECK_NEAR(c.x, 1.0f);
        CHECK_NEAR(c.y, 1.0f);
        CHECK_NEAR(c.z, 1.0f);
    }

    // 7. Color modulates per channel: pure-red light produces only a red
    // contribution (green and blue channels are zero).
    {
        PointLight light;
        light.position = Vec3{2.5f, 0.0f, 0.0f};
        light.range = 5.0f;
        light.intensity = 1.0f;
        light.color = Vec3{1.0f, 0.0f, 0.0f};
        Vec3 fragPos{0.0f, 0.0f, 0.0f};
        Vec3 normal{1.0f, 0.0f, 0.0f};
        Vec3 c = pointLightContribution(light, fragPos, normal);
        CHECK_NEAR(c.x, 0.5f);
        CHECK_NEAR(c.y, 0.0f);
        CHECK_NEAR(c.z, 0.0f);
    }

    return iron_test_result();
}
