#include "render/ReflectionProbe.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace iron;

static bool approx(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
static bool approx3(Vec3 a, Vec3 b, float eps = 1e-3f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

static GpuReflectionProbe makeBox(Vec3 center, Vec3 half, CubemapHandle h = 1) {
    return GpuReflectionProbe{ {center.x - half.x, center.y - half.y, center.z - half.z},
                               {center.x + half.x, center.y + half.y, center.z + half.z},
                               center, h };
}

int main() {
    // --- nearest-probe selection ---
    std::vector<GpuReflectionProbe> probes = {
        makeBox({0, 0, 0}, {5, 5, 5}, 10),
        makeBox({20, 0, 0}, {3, 3, 3}, 20),
    };
    assert(nearestProbeContaining(probes, Vec3{1, 1, 1}) == 0);
    assert(nearestProbeContaining(probes, Vec3{20, 0, 0}) == 1);
    assert(nearestProbeContaining(probes, Vec3{100, 0, 0}) == -1);
    assert(nearestProbeContaining({}, Vec3{0, 0, 0}) == -1);

    std::vector<GpuReflectionProbe> overlap = {
        makeBox({0, 0, 0}, {10, 10, 10}, 1),
        makeBox({5, 0, 0}, {10, 10, 10}, 2),
    };
    assert(nearestProbeContaining(overlap, Vec3{4, 0, 0}) == 1);

    // --- box projection ---
    GpuReflectionProbe p = makeBox({0, 0, 0}, {10, 10, 10});
    Vec3 corrected = boxProjectReflection(Vec3{1, 0, 0}, Vec3{0, 0, 0}, p);
    assert(approx3(normalize(corrected), Vec3{1, 0, 0}));

    Vec3 c2 = normalize(boxProjectReflection(Vec3{1, 0, 0}, Vec3{0, 5, 0}, p));
    assert(c2.y > 0.0f);

    // --- cube-face view matrices ---
    for (int face = 0; face < 6; ++face) {
        Mat4 v = cubeFaceView(Vec3{0, 0, 0}, face);
        (void)v;
    }
    Mat4 vpx = cubeFaceView(Vec3{0, 0, 0}, 0);
    assert(!std::isnan(vpx.m[0]));

    std::printf("test_reflection_probe: OK\n");
    return 0;
}
