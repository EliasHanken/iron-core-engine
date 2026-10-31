// Tests for the fluid's fixed-step clock, and compile-checks of its solver.
#include "render/FluidStep.h"

#include <cassert>
#include <cmath>
#include <cstdio>

#ifdef IRON_RENDER_BACKEND_VULKAN
#include "render/FluidVolume.h"
#include "render/backends/vulkan/VkShader.h"
#endif

namespace {

void testOneFrameIsOneStep() {
    float accumulator = 0.0f;
    assert(iron::fluidStepsThisFrame(iron::kFluidStepSeconds, accumulator) == 1);
    assert(accumulator < 1e-4f);
}

// Offline capture steps exactly once a frame, forever, or determinism is lost.
void testFixedDtNeverDrifts() {
    float accumulator = 0.0f;
    for (int i = 0; i < 10000; ++i) {
        assert(iron::fluidStepsThisFrame(iron::kFluidStepSeconds, accumulator) == 1);
    }
}

void testFastFramesAccumulate() {
    float accumulator = 0.0f;
    assert(iron::fluidStepsThisFrame(1.0f / 120.0f, accumulator) == 0);
    assert(iron::fluidStepsThisFrame(1.0f / 120.0f, accumulator) == 1);
}

void testStallDoesNotQueueWork() {
    float accumulator = 0.0f;
    assert(iron::fluidStepsThisFrame(1.0f, accumulator) == iron::kMaxFluidStepsPerFrame);
    assert(accumulator < iron::kFluidStepSeconds);
}

#ifdef IRON_RENDER_BACKEND_VULKAN
void testSolverCompiles() {
    iron::FluidVolumeDesc desc;
    desc.sourceGlsl =
        "void fluidSource(vec3 world, inout vec3 velocity, inout float density, inout float heat) {\n"
        "    density += fluid.dt;\n"
        "}\n";
    desc.obstacleGlsl = "float fluidObstacle(vec3 world) { return length(world) - 1.0; }\n";

    const auto sources = iron::composeFluidSources(desc);
    assert(sources.size() == 10);
    for (const std::string& source : sources) {
        assert(!iron::compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, source).empty());
    }
}
#endif

}  // namespace

int main() {
    testOneFrameIsOneStep();
    testFixedDtNeverDrifts();
    testFastFramesAccumulate();
    testStallDoesNotQueueWork();
#ifdef IRON_RENDER_BACKEND_VULKAN
    testSolverCompiles();
#endif
    std::puts("test_fluid_step: all passed");
    return 0;
}
