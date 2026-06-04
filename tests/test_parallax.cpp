#include "render/Parallax.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace iron;

static bool approx2(Vec2 a, Vec2 b, float eps = 1e-3f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps;
}

int main() {
    // Flat height field at peak (1.0 everywhere) => depth 0 => no offset.
    auto flatPeak = [](Vec2) { return 1.0f; };
    Vec2 off = parallaxOcclusionOffset(flatPeak, Vec2{0.5f, 0.5f}, Vec3{0.0f, 0.0f, 1.0f}, 0.1f);
    assert(approx2(off, Vec2{0.5f, 0.5f}));

    // Head-on view with zero lateral component => no lateral shift regardless of depth.
    auto fullDepth = [](Vec2) { return 0.0f; };
    Vec2 off2 = parallaxOcclusionOffset(fullDepth, Vec2{0.5f, 0.5f}, Vec3{0.0f, 0.0f, 1.0f}, 0.1f);
    assert(approx2(off2, Vec2{0.5f, 0.5f}));

    // View tilted toward +U on a full-depth field: UV shifts in -U, V unchanged.
    Vec3 vt = Vec3{0.6f, 0.0f, 0.8f};
    Vec2 off3 = parallaxOcclusionOffset(fullDepth, Vec2{0.5f, 0.5f}, vt, 0.1f);
    assert(off3.x < 0.5f - 1e-3f);
    assert(std::fabs(off3.y - 0.5f) < 1e-4f);

    // Grazing view uses >= as many layers as head-on (adaptive step count).
    assert(parallaxLayerCount(Vec3{0.0f, 0.0f, 1.0f}) <= parallaxLayerCount(Vec3{0.9f, 0.0f, 0.1f}));

    std::printf("test_parallax: OK\n");
    return 0;
}
