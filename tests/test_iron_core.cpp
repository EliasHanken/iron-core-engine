// Tests for Iron Core: the film's timeline, the generated rock and shell, and a
// compile-check of every effect.
#include "IronScene.h"
#include "IronTimeline.h"

#include <cassert>
#include <cmath>
#include <cstdio>

#ifdef IRON_RENDER_BACKEND_VULKAN
#include "IronShaders.h"
#include "render/backends/vulkan/VkShader.h"
#endif

using iron::Vec3;
using namespace ironcore;

namespace {

bool inUnitRange(float v) { return v >= 0.0f && v <= 1.0f; }

bool isFinite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void testPhasesStayInRange() {
    for (int i = 0; i <= 3000; ++i) {
        const float t = kFilmSeconds * static_cast<float>(i) / 3000.0f;
        const Beat b = evaluateBeat(t);
        assert(inUnitRange(b.veins) && inUnitRange(b.lightning) && inUnitRange(b.ignition));
        assert(inUnitRange(b.flash) && inUnitRange(b.rebirth) && inUnitRange(b.title));
        assert(isFinite(b.cameraEye) && isFinite(b.cameraTarget));
    }
}

void testStoryBeats() {
    assert(evaluateBeat(kIgnitionTime - 0.01f).ignition == 0.0f);
    assert(evaluateBeat(kIgnitionTime + 1.0f).ignition == 1.0f);
    assert(evaluateBeat(5.0f).veins == 0.0f);
    assert(evaluateBeat(20.0f).rebirth == 0.0f);
    assert(evaluateBeat(kFilmSeconds).rebirth == 1.0f);
    assert(evaluateBeat(25.0f).title == 0.0f);
    assert(evaluateBeat(kFilmSeconds).title == 1.0f);
}

void testShockNeverShrinks() {
    float previous = 0.0f;
    for (int i = 0; i <= 1000; ++i) {
        const float t = kIgnitionTime + 14.0f * static_cast<float>(i) / 1000.0f;
        const float radius = evaluateBeat(t).shockRadius;
        assert(radius >= previous);
        previous = radius;
    }
    assert(evaluateBeat(kIgnitionTime - 1.0f).shockRadius == 0.0f);
}

void testShockArrivalMatchesItsRadius() {
    for (float radius : {4.0f, 10.0f, 20.0f}) {
        assert(std::abs(shockRadiusAt(shockArrival(radius)) - radius) < 0.01f);
    }
}

void testCameraIsContinuous() {
    Vec3 previous = evaluateBeat(0.0f).cameraEye;
    // Skips the blast, where the shake is deliberate.
    for (float t = 0.01f; t < kIgnitionTime; t += 0.01f) {
        const Vec3 eye = evaluateBeat(t).cameraEye;
        assert(iron::length(eye - previous) < 0.1f);
        previous = eye;
    }
}

void testRocksAreDeterministicAndFinite() {
    const auto a = makeRockField(4242u, kRockCount);
    const auto b = makeRockField(4242u, kRockCount);
    assert(a.size() == static_cast<std::size_t>(kRockCount));
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (float t = 0.0f; t <= kFilmSeconds; t += 1.0f) {
            const Vec3 p = rockPosition(a[i], t);
            const Vec3 q = rockPosition(b[i], t);
            assert(isFinite(p));
            assert(p.x == q.x && p.y == q.y && p.z == q.z);
        }
    }
}

void testRocksEndInTheRings() {
    for (const RockInstance& rock : makeRockField(4242u, kRockCount)) {
        const float radius = iron::length(rockPosition(rock, kFilmSeconds));
        assert(std::abs(radius - rock.ringRadius) < 0.01f);
    }
}

void assertMeshIsValid(const iron::MeshData& mesh) {
    assert(!mesh.vertices.empty());
    assert(mesh.indices.size() % 3 == 0);
    for (std::uint32_t index : mesh.indices) {
        (void)index;
        assert(index < mesh.vertices.size());
    }
    for (const iron::Vertex& v : mesh.vertices) {
        (void)v;
        assert(isFinite(v.position));
        assert(std::abs(iron::length(v.normal) - 1.0f) < 1e-3f);
    }
}

void testRockMesh() { assertMeshIsValid(makeRock(1u)); }

void testShellCoversTheSphere() {
    std::vector<RockInstance> plates;
    const auto meshes = makeShellPlates(kShellPlates, kCoreRadius, 7u, plates);
    assert(meshes.size() == static_cast<std::size_t>(kShellPlates));
    assert(plates.size() == meshes.size());

    Vec3 average{0, 0, 0};
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        assertMeshIsValid(meshes[i]);
        assert(plates[i].plate);
        average = average + plates[i].home;
    }
    // Plates spread evenly around the core leave their average near the centre.
    assert(iron::length(average) * (1.0f / kShellPlates) < 0.5f);
}

void testShellStaysStillUntilIgnition() {
    std::vector<RockInstance> plates;
    makeShellPlates(kShellPlates, kCoreRadius, 7u, plates);
    for (const RockInstance& plate : plates) {
        const Vec3 p = rockPosition(plate, kIgnitionTime - 0.1f);
        assert(iron::length(p - plate.home) < 1e-5f);
        assert(rockAngle(plate, kIgnitionTime - 0.1f) == 0.0f);
    }
}

void testLightVisibility() {
    const Vec3 eye{0, 0, 20};
    const Vec3 light{0, 0, 0};

    assert(lightVisibility(eye, light, 3.0f, {}) == 1.0f);

    // A sphere filling the view hides everything, and one off to the side nothing.
    const std::vector<iron::Vec4> full = {{0, 0, 10, 6}};
    assert(lightVisibility(eye, light, 3.0f, full) == 0.0f);
    const std::vector<iron::Vec4> aside = {{30, 0, 10, 2}};
    assert(lightVisibility(eye, light, 3.0f, aside) == 1.0f);

    // A small one partly covers it.
    const std::vector<iron::Vec4> part = {{1.5f, 0, 10, 1}};
    const float seen = lightVisibility(eye, light, 3.0f, part);
    assert(seen > 0.0f && seen < 1.0f);

    // Something behind the light can't block it.
    const std::vector<iron::Vec4> behind = {{0, 0, -10, 6}};
    assert(lightVisibility(eye, light, 3.0f, behind) == 1.0f);
}

#ifdef IRON_RENDER_BACKEND_VULKAN
void checkCompiles(const iron::GpuParticlesDesc& desc) {
    const iron::GpuParticleSources s = iron::composeGpuParticleSources(desc);
    assert(!iron::compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, s.compute).empty());
    assert(!iron::compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, s.vertex).empty());
    assert(!iron::compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, s.fragment).empty());
    if (!s.inject.empty()) {
        assert(!iron::compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, s.inject).empty());
    }
}

void testEffectsCompile() {
    checkCompiles(makeStarEffect());
    checkCompiles(makeDebrisEffect());
    checkCompiles(makeSkinEffect());
    checkCompiles(makeFieldEffect());
    checkCompiles(makeArcEffect());
    checkCompiles(makeShockEffect());
    checkCompiles(makeEmberEffect());
    checkCompiles(makeFlareEffect());
}

void testFluidCompiles() {
    for (const std::string& source : iron::composeFluidSources(makeFluidDesc())) {
        assert(!iron::compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, source).empty());
    }
}
#endif

}  // namespace

int main() {
    testPhasesStayInRange();
    testStoryBeats();
    testShockNeverShrinks();
    testShockArrivalMatchesItsRadius();
    testCameraIsContinuous();
    testRocksAreDeterministicAndFinite();
    testRocksEndInTheRings();
    testRockMesh();
    testShellCoversTheSphere();
    testShellStaysStillUntilIgnition();
    testLightVisibility();
#ifdef IRON_RENDER_BACKEND_VULKAN
    testEffectsCompile();
    testFluidCompiles();
#endif
    std::puts("test_iron_core: all passed");
    return 0;
}
