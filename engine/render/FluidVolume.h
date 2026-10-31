#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "iron::FluidVolume requires -DIRON_RENDER_BACKEND=vulkan"
#endif

#include "render/FluidStep.h"
#include "render/VolumeBounds.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace iron {

class Renderer;

// A programmable GPU fluid: an incompressible solver carrying density and heat
// on 3D grids. The engine owns the solver; what feeds it is game GLSL.
//
// `commonGlsl` is pasted into both game stages. If paramsSize > 0, declare the
// matching block there (std140):
//   layout(set = 0, binding = 2) uniform Params { ... } params;
//
// Each stage also sees a `fluid` block with `dt`, `time` and `maxSpeed`, and
// provides one function:
//   sourceGlsl:   void fluidSource(vec3 world, inout vec3 velocity,
//                                  inout float density, inout float heat);
//   obstacleGlsl: float fluidObstacle(vec3 world);   // negative inside a solid
//
// Velocity is in metres per second. fluidSource runs for every voxel on every
// step, so it applies forces itself, scaled by fluid.dt.
struct FluidVolumeDesc {
    VolumeBounds bounds;
    VolumeSize   size{192, 192, 128};
    int          pressureIterations = 24;
    float        vorticity = 6.0f;
    float        velocityDissipation = 0.2f;   // Per second.
    float        maxSpeed = 60.0f;             // Metres per second.
    int          obstacleInterval = 4;         // Re-evaluate solids every N steps.
    std::size_t  paramsSize = 0;
    std::string  commonGlsl;
    std::string  sourceGlsl;
    std::string  obstacleGlsl;
};

class FluidVolume {
public:
    virtual ~FluidVolume() = default;

    virtual void setParams(const void* params) = 0;
    virtual void setDissipation(float densityPerSecond) = 0;

    // Advances one kFluidStepSeconds step. Call between Renderer::beginFrame and
    // endFrame, and any number of times per frame.
    virtual void step() = 0;

    // Empties the grids on the next step.
    virtual void reset() = 0;

    virtual const VolumeBounds& bounds() const = 0;
};

// Every compute shader the solver uses, in dispatch order. Exposed so tests can
// compile-check a fluid without a GPU.
std::vector<std::string> composeFluidSources(const FluidVolumeDesc& desc);

// Returns nullptr on failure. Must be destroyed before the Renderer.
std::unique_ptr<FluidVolume> createFluidVolume(Renderer& renderer, const FluidVolumeDesc& desc);

}  // namespace iron
