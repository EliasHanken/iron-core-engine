#include "render/TessLod.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace iron;

int main() {
    const float target = 0.1f;
    const float maxF   = 64.0f;

    // Both endpoints behind the near plane (w<=0) => clamp to max.
    assert(tessEdgeFactor(Vec4{0, 0, -1, -0.5f}, Vec4{0, 0, -1, -0.5f}, target, maxF) == maxF);

    // Short on-screen edge (NDC length 0.02 < target) => factor 1.
    float fShort = tessEdgeFactor(Vec4{0.0f, 0, 0, 1}, Vec4{0.02f, 0, 0, 1}, target, maxF);
    assert(std::fabs(fShort - 1.0f) < 1e-4f);

    // Long on-screen edge (NDC length 0.8 = 8*target) => factor 8.
    float fLong = tessEdgeFactor(Vec4{-0.4f, 0, 0, 1}, Vec4{0.4f, 0, 0, 1}, target, maxF);
    assert(std::fabs(fLong - 8.0f) < 1e-3f);

    // Nearer edge (smaller w => larger NDC span) tessellates more than far.
    float fNear = tessEdgeFactor(Vec4{-0.1f, 0, 0, 1.0f}, Vec4{0.1f, 0, 0, 1.0f}, target, maxF);
    float fFar  = tessEdgeFactor(Vec4{-0.1f, 0, 0, 5.0f}, Vec4{0.1f, 0, 0, 5.0f}, target, maxF);
    assert(fNear > fFar);

    // Never exceeds maxF.
    assert(tessEdgeFactor(Vec4{-1.0f, 0, 0, 1}, Vec4{1.0f, 0, 0, 1}, target, 4.0f) == 4.0f);

    std::printf("test_tess_lod: OK\n");
    return 0;
}
