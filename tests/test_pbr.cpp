// CPU port of the PBR BRDF helpers (kept in lockstep with the GLSL in
// StandardLitShader.h). Verifies F0, Fresnel, GGX-D, and Smith-G properties.
#include "render/Pbr.h"

#include <cassert>
#include <cstdio>

using iron::Vec3;

static bool approx(float a, float b, float eps = 1e-4f) {
    float d = a - b; return (d < 0 ? -d : d) <= eps;
}

int main() {
    // Dielectric F0 ~= 0.04 (per channel).
    {
        Vec3 f0 = iron::f0For(Vec3{0.8f, 0.1f, 0.1f}, 0.0f);
        assert(approx(f0.x, 0.04f) && approx(f0.y, 0.04f) && approx(f0.z, 0.04f));
    }
    // Metallic F0 == albedo.
    {
        Vec3 albedo{0.8f, 0.1f, 0.1f};
        Vec3 f0 = iron::f0For(albedo, 1.0f);
        assert(approx(f0.x, albedo.x) && approx(f0.y, albedo.y) && approx(f0.z, albedo.z));
    }
    // Fresnel: at normal incidence (cos=1) returns F0; at grazing (cos=0) -> ~1.
    {
        Vec3 f0{0.04f, 0.04f, 0.04f};
        Vec3 at0 = iron::fresnelSchlick(1.0f, f0);
        Vec3 at90 = iron::fresnelSchlick(0.0f, f0);
        assert(approx(at0.x, 0.04f));
        assert(at90.x > 0.99f);
    }
    // GGX D: at fixed NdotH=1, lower roughness => higher (sharper) peak.
    {
        float dRough = iron::distributionGGX(1.0f, 0.8f);
        float dSharp = iron::distributionGGX(1.0f, 0.1f);
        assert(dSharp > dRough);
    }
    // Smith G in [0,1] and increases with NdotV/NdotL.
    {
        float g = iron::geometrySmith(0.9f, 0.9f, 0.5f);
        assert(g >= 0.0f && g <= 1.0f);
        float gLow = iron::geometrySmith(0.2f, 0.2f, 0.5f);
        assert(g > gLow);
    }
    std::puts("test_pbr: all passed");
    return 0;
}
