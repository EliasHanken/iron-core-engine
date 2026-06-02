// Unit tests for the ACES filmic tonemap CPU port (mirrors the GLSL fit used
// by the composite shaders). Verifies the curve maps 0->0, is monotonic,
// saturates toward 1.0, hits a known midpoint, and that exposure scales input.
#include "render/Tonemap.h"

#include <cassert>
#include <cstdio>

using iron::Vec3;
using iron::acesFilmic;

static bool approx(float a, float b, float eps = 1e-4f) {
    float d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

int main() {
    // 1. Black maps to black.
    {
        Vec3 r = acesFilmic(Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
        assert(approx(r.x, 0.0f) && approx(r.y, 0.0f) && approx(r.z, 0.0f));
    }

    // 2. Monotonic increasing on a grey ramp.
    {
        float prev = -1.0f;
        for (int i = 0; i <= 20; ++i) {
            float v = static_cast<float>(i) * 0.5f;  // 0..10
            float out = acesFilmic(Vec3{v, v, v}, 1.0f).x;
            assert(out >= prev - 1e-6f);
            prev = out;
        }
    }

    // 3. Saturates toward 1.0 for very bright input (never exceeds 1).
    {
        Vec3 r = acesFilmic(Vec3{1000.0f, 1000.0f, 1000.0f}, 1.0f);
        assert(r.x <= 1.0f && r.x > 0.95f);
    }

    // 4. Known midpoint: Narkowicz ACES at x=1.0 -> ~0.8.
    //    (2.51+0.03)/(2.43+0.59+0.14) = 2.54/3.16 = 0.803797...
    {
        float out = acesFilmic(Vec3{1.0f, 1.0f, 1.0f}, 1.0f).x;
        assert(approx(out, 0.803797f, 1e-3f));
    }

    // 5. Exposure scales the input before the curve: exposure 2 on x=0.5
    //    equals exposure 1 on x=1.0.
    {
        float a = acesFilmic(Vec3{0.5f, 0.5f, 0.5f}, 2.0f).x;
        float b = acesFilmic(Vec3{1.0f, 1.0f, 1.0f}, 1.0f).x;
        assert(approx(a, b, 1e-4f));
    }

    std::puts("test_tonemap: all passed");
    return 0;
}
