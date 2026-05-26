// CPU port of the curl-noise functions from the M10 particle compute
// shader. Validates determinism, boundedness, reproducibility, and
// that curl is (approximately) divergence-free.
//
// The shader itself runs on the GPU; this test is the only place these
// math constants are checked. Pure C++ — builds + passes under both
// render backends.

#include "test_framework.h"

#include <cmath>

namespace {

float fract(float x) { return x - std::floor(x); }

float hash11(float n) {
    return fract(std::sin(n) * 43758.5453f);
}

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
V3 operator*(V3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }

float dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
V3 frac(V3 p) { return {fract(p.x), fract(p.y), fract(p.z)}; }
V3 floorv(V3 p) { return {std::floor(p.x), std::floor(p.y), std::floor(p.z)}; }

float hash31(V3 p) {
    p = frac(V3{p.x * 443.897f, p.y * 441.423f, p.z * 437.195f});
    V3 yzx = {p.y, p.z, p.x};
    float d = dot(p, V3{yzx.x + 19.19f, yzx.y + 19.19f, yzx.z + 19.19f});
    p = V3{p.x + d, p.y + d, p.z + d};
    return fract((p.x + p.y) * p.z);
}

float vnoise(V3 p) {
    V3 i = floorv(p);
    V3 f = frac(p);
    f = V3{f.x*f.x*(3.0f - 2.0f*f.x),
           f.y*f.y*(3.0f - 2.0f*f.y),
           f.z*f.z*(3.0f - 2.0f*f.z)};
    auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    float n000 = hash31(i + V3{0,0,0});
    float n100 = hash31(i + V3{1,0,0});
    float n010 = hash31(i + V3{0,1,0});
    float n110 = hash31(i + V3{1,1,0});
    float n001 = hash31(i + V3{0,0,1});
    float n101 = hash31(i + V3{1,0,1});
    float n011 = hash31(i + V3{0,1,1});
    float n111 = hash31(i + V3{1,1,1});
    return mix(
        mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
        mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
        f.z);
}

V3 potential(V3 p) {
    return V3{
        vnoise(p),
        vnoise(p + V3{31.42f, 17.13f, 95.11f}),
        vnoise(p + V3{7.31f,  81.97f, 49.18f})};
}

V3 curl(V3 p) {
    constexpr float eps = 0.01f;
    V3 dPdx = (potential(p + V3{eps, 0, 0}) - potential(p - V3{eps, 0, 0})) * (1.0f / (2.0f * eps));
    V3 dPdy = (potential(p + V3{0, eps, 0}) - potential(p - V3{0, eps, 0})) * (1.0f / (2.0f * eps));
    V3 dPdz = (potential(p + V3{0, 0, eps}) - potential(p - V3{0, 0, eps})) * (1.0f / (2.0f * eps));
    return V3{
        dPdy.z - dPdz.y,
        dPdz.x - dPdx.z,
        dPdx.y - dPdy.x};
}

float div_curl(V3 p) {
    constexpr float eps = 0.01f;
    V3 dx = (curl(p + V3{eps, 0, 0}) - curl(p - V3{eps, 0, 0})) * (1.0f / (2.0f * eps));
    V3 dy = (curl(p + V3{0, eps, 0}) - curl(p - V3{0, eps, 0})) * (1.0f / (2.0f * eps));
    V3 dz = (curl(p + V3{0, 0, eps}) - curl(p - V3{0, 0, eps})) * (1.0f / (2.0f * eps));
    return dx.x + dy.y + dz.z;
}

}  // namespace

int main() {
    // Determinism: vnoise(p) returns the same value across two calls.
    {
        const V3 p{0.37f, 1.21f, 4.95f};
        const float a = vnoise(p);
        const float b = vnoise(p);
        CHECK(a == b);
    }

    // Bounded: vnoise(p) ∈ [0, 1] for a grid of sample points.
    {
        bool inRange = true;
        for (int gx = 0; gx < 5; ++gx)
        for (int gy = 0; gy < 5; ++gy)
        for (int gz = 0; gz < 5; ++gz) {
            const V3 p{gx * 0.7f, gy * 0.9f, gz * 1.1f};
            const float v = vnoise(p);
            if (v < 0.0f || v > 1.0f) { inRange = false; break; }
        }
        CHECK(inRange);
    }

    // Reproducibility: curl(p) returns identical values across two calls.
    {
        const V3 p{1.0f, 2.0f, 3.0f};
        const V3 a = curl(p);
        const V3 b = curl(p);
        CHECK_NEAR(a.x, b.x);
        CHECK_NEAR(a.y, b.y);
        CHECK_NEAR(a.z, b.z);
    }

    // Curl is (approximately) divergence-free. div(curl(p)) should be
    // near zero (mathematically exact; numerically bounded by our eps).
    {
        const V3 points[] = {
            {0.5f, 1.5f, 2.5f},
            {3.0f, 0.0f, -1.0f},
            {-2.5f, 4.2f, 0.7f},
        };
        bool nearZero = true;
        for (const V3& p : points) {
            const float d = div_curl(p);
            if (std::fabs(d) > 0.5f) { nearZero = false; break; }
        }
        CHECK(nearZero);
    }

    return iron_test_result();
}
