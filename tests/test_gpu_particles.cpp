// Compile-checks the engine's generated particle and volume shaders. These need
// no GPU: they prove the composed GLSL is valid.
#include "render/GpuParticles.h"
#include "render/backends/vulkan/VkShader.h"

#include <cassert>
#include <cstdio>

namespace {

iron::GpuParticlesDesc minimalEffect() {
    iron::GpuParticlesDesc desc;
    desc.count = 16;
    desc.simulateGlsl = "void particleSimulate(inout Particle p, uint id) {}\n";
    desc.vertexGlsl =
        "void particleVertex(Particle p, uint id, vec2 corner,\n"
        "                    out vec4 clipPos, out vec4 color, out vec4 data) {\n"
        "    clipPos = vec4(corner, 0.0, 1.0); color = vec4(1.0); data = vec4(0.0);\n"
        "}\n";
    desc.fragmentGlsl =
        "vec4 particleFragment(vec2 corner, vec4 color, vec4 data) { return color; }\n";
    return desc;
}

bool compiles(VkShaderStageFlagBits stage, const std::string& source) {
    return !iron::compileGlsl(stage, source).empty();
}

void testEffectWithoutEmissionHasNoInjectShader() {
    const iron::GpuParticleSources s = iron::composeGpuParticleSources(minimalEffect());
    assert(s.inject.empty());
    assert(compiles(VK_SHADER_STAGE_COMPUTE_BIT, s.compute));
    assert(compiles(VK_SHADER_STAGE_VERTEX_BIT, s.vertex));
    assert(compiles(VK_SHADER_STAGE_FRAGMENT_BIT, s.fragment));
}

void testEmissionComposesInjectShader() {
    iron::GpuParticlesDesc desc = minimalEffect();
    desc.emissionGlsl = "vec3 particleEmission(Particle p, uint id) { return p.a.rgb; }\n";
    const iron::GpuParticleSources s = iron::composeGpuParticleSources(desc);
    assert(!s.inject.empty());
    assert(compiles(VK_SHADER_STAGE_COMPUTE_BIT, s.inject));
}

}  // namespace

int main() {
    testEffectWithoutEmissionHasNoInjectShader();
    testEmissionComposesInjectShader();
    std::puts("test_gpu_particles: all passed");
    return 0;
}
