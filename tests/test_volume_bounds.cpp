// Unit tests for the world-to-grid math shared by the volume systems.
#include "render/VolumeBounds.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using iron::Vec3;
using iron::VolumeBounds;
using iron::VolumeSize;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
bool near(Vec3 a, Vec3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); }

const VolumeBounds kBox{{-12, -12, -8}, {12, 12, 8}};

void testWorldToVolume() {
    assert(near(iron::worldToVolume(kBox, kBox.min), Vec3{0, 0, 0}));
    assert(near(iron::worldToVolume(kBox, kBox.max), Vec3{1, 1, 1}));
    assert(near(iron::worldToVolume(kBox, Vec3{0, 0, 0}), Vec3{0.5f, 0.5f, 0.5f}));
}

void testVoxelSize() {
    assert(near(iron::volumeVoxelSize(kBox, VolumeSize{192, 192, 128}),
                Vec3{0.125f, 0.125f, 0.125f}));
}

void testRayFromOutside() {
    float t0 = 0.0f, t1 = 0.0f;
    assert(iron::rayVolumeInterval(kBox, Vec3{-20, 0, 0}, Vec3{1, 0, 0}, t0, t1));
    assert(near(t0, 8.0f));
    assert(near(t1, 32.0f));
}

// The camera flies through the box, so a ray must start at the camera.
void testRayFromInside() {
    float t0 = 1.0f, t1 = 0.0f;
    assert(iron::rayVolumeInterval(kBox, Vec3{0, 0, 0}, Vec3{0, 0, 1}, t0, t1));
    assert(t0 == 0.0f);
    assert(near(t1, 8.0f));
}

void testRayMisses() {
    float t0 = 0.0f, t1 = 0.0f;
    assert(!iron::rayVolumeInterval(kBox, Vec3{-20, 0, 0}, Vec3{-1, 0, 0}, t0, t1));
    assert(!iron::rayVolumeInterval(kBox, Vec3{-20, 30, 0}, Vec3{1, 0, 0}, t0, t1));
}

// A zero direction component must not divide by zero.
void testAxisParallelRay() {
    float t0 = 0.0f, t1 = 0.0f;
    assert(iron::rayVolumeInterval(kBox, Vec3{-20, 5, 2}, Vec3{1, 0, 0}, t0, t1));
    assert(std::isfinite(t0) && std::isfinite(t1));
}

}  // namespace

int main() {
    testWorldToVolume();
    testVoxelSize();
    testRayFromOutside();
    testRayFromInside();
    testRayMisses();
    testAxisParallelRay();
    std::puts("test_volume_bounds: all passed");
    return 0;
}
