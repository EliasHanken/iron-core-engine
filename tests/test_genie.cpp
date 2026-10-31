// Tests for the genie demo: the shot timeline, the generated scene, and a
// compile-check of every effect's shaders (Vulkan backend only).
#include "GenieScene.h"
#include "GenieTimeline.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

#ifdef IRON_RENDER_BACKEND_VULKAN
#include "GenieShaders.h"
#include "OrbShaders.h"
#include "render/backends/vulkan/VkShader.h"
#endif

using iron::Vec3;

namespace {

bool inUnitRange(float v) { return v >= 0.0f && v <= 1.0f; }

bool isFinite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void testShotPhasesStayInRange() {
    constexpr int kSamples = 2600;
    for (int i = 0; i < kSamples; ++i) {
        const float t = genie::kLoopSeconds * static_cast<float>(i) / kSamples;
        const genie::Shot s = genie::evaluateShot(t);
        assert(inUnitRange(s.stream));
        assert(inUnitRange(s.smoke));
        assert(inUnitRange(s.reveal));
        assert(inUnitRange(s.dissolve));
        assert(inUnitRange(s.eyes));
        assert(isFinite(s.cameraEye));
        assert(isFinite(s.cameraTarget));
    }
}

void testStoryBeats() {
    const genie::Shot start = genie::evaluateShot(0.0f);
    assert(start.stream == 0.0f && start.smoke == 0.0f && start.reveal == 0.0f);

    const genie::Shot awake = genie::evaluateShot(15.0f);
    assert(awake.stream == 1.0f && awake.reveal == 1.0f && awake.eyes == 1.0f);
    assert(awake.dissolve == 0.0f);

    // The eyes must open only once the head exists.
    assert(genie::evaluateShot(11.0f).eyes == 0.0f);
    assert(genie::evaluateShot(11.0f).reveal > 0.95f);
}

void testShotLoopsSeamlessly() {
    const genie::Shot a = genie::evaluateShot(0.0f);
    const genie::Shot b = genie::evaluateShot(genie::kLoopSeconds - 0.001f);
    assert(b.stream == a.stream);
    assert(b.smoke == a.smoke);
    assert(b.eyes == a.eyes);
    // Reveal and dissolve both end at 1: a fully dissolved body shows nothing,
    // matching reveal 0 at the start.
    assert(b.dissolve == 1.0f);
    assert(iron::length(b.cameraTarget - a.cameraTarget) < 0.01f);
}

void testLampRestsOnTheSand() {
    const Vec3 lamp = genie::lampPosition();
    assert(lamp.y > genie::groundHeight(lamp.x, lamp.z));

    const Vec3 tip = genie::spoutTip();
    assert(tip.x > lamp.x);
    assert(tip.y > genie::groundHeight(tip.x, tip.z));
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
        assert(isFinite(v.normal));
        assert(std::abs(iron::length(v.normal) - 1.0f) < 1e-3f);
    }
}

void testGeneratedMeshes() {
    const iron::MeshData dunes = genie::makeDunes();
    assertMeshIsValid(dunes);
    for (const iron::Vertex& v : dunes.vertices) {
        (void)v;
        assert(v.position.y == genie::groundHeight(v.position.x, v.position.z));
        assert(v.normal.y > 0.0f);
    }

    assertMeshIsValid(genie::makeLamp());
}

void testLampNormalsPointOutward() {
    const iron::MeshData lamp = genie::makeLamp();
    // Sampled at the belly's sides: at x = 0 only the belly reaches this far from
    // the axis along Z, since the foot and lid are both narrower.
    constexpr float kOnlyBellyReaches = 0.30f;
    int checked = 0;
    for (const iron::Vertex& v : lamp.vertices) {
        if (std::abs(v.position.x) < 0.01f && std::abs(v.position.z) > kOnlyBellyReaches) {
            assert(v.normal.z * v.position.z > 0.0f);
            ++checked;
        }
    }
    assert(checked > 0);
}

void testSandNormalMapTiles() {
    constexpr int kSize = 64;
    const auto pixels = genie::makeSandNormalMap(kSize);
    assert(pixels.size() == kSize * kSize * 4);

    // Neighbouring rows differ by a similar amount across the wrap as they do
    // inside the image; a seam would show up as a much larger jump.
    const auto rowDelta = [&](int a, int b) {
        int delta = 0;
        for (int x = 0; x < kSize; ++x) {
            const std::size_t ia = (static_cast<std::size_t>(a) * kSize + x) * 4;
            const std::size_t ib = (static_cast<std::size_t>(b) * kSize + x) * 4;
            delta = std::max(delta, std::abs(pixels[ia + 1] - pixels[ib + 1]));
        }
        return delta;
    };
    int largestInside = 0;
    for (int y = 0; y + 1 < kSize; ++y) largestInside = std::max(largestInside, rowDelta(y, y + 1));
    assert(rowDelta(kSize - 1, 0) <= largestInside + 2);
}

#ifdef IRON_RENDER_BACKEND_VULKAN
void checkCompiles(const iron::GpuParticlesDesc& desc) {
    const iron::GpuParticleSources s = iron::composeGpuParticleSources(desc);
    assert(!iron::compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, s.compute).empty());
    assert(!iron::compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, s.vertex).empty());
    assert(!iron::compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, s.fragment).empty());
}

void testEffectsCompile() {
    checkCompiles(genie::makeBodyEffect());
    checkCompiles(genie::makePlasmaEffect());
    checkCompiles(genie::makeSmokeEffect());
    checkCompiles(genie::makeGlowEffect());
    checkCompiles(genie::makeOrbEffect());
    checkCompiles(genie::makeGridEffect());
}
#endif

}  // namespace

int main() {
    testShotPhasesStayInRange();
    testStoryBeats();
    testShotLoopsSeamlessly();
    testLampRestsOnTheSand();
    testGeneratedMeshes();
    testLampNormalsPointOutward();
    testSandNormalMapTiles();
#ifdef IRON_RENDER_BACKEND_VULKAN
    testEffectsCompile();
#endif
    std::puts("test_genie: all passed");
    return 0;
}
