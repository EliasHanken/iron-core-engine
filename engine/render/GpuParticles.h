#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "iron::GpuParticles requires -DIRON_RENDER_BACKEND=vulkan"
#endif

#include "math/Mat4.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace iron {

class FluidVolume;
class LightVolume;
class Renderer;

enum class ParticleBlend {
    Additive,            // Light-emitting: plasma, sparks, glow.
    PremultipliedAlpha,  // Light-blocking: smoke, dust.
};

// A programmable GPU particle effect. The engine owns the buffers, pipelines,
// barriers and draw; the effect's behaviour and look are GLSL supplied by the
// caller, so a new effect never needs an engine change.
//
// Every stage sees:
//   struct Particle { vec4 position; vec4 velocity; vec4 a; vec4 b; };
//     By convention position.w = age, velocity.w = lifetime; a/b are free.
//     Particles start zeroed, so lifetime 0 means "not spawned yet".
//   frame.view, frame.projection, frame.cameraPos  (vertex/fragment only)
//   frame.dt, frame.time, frame.count, frame.frameIndex
//
// `commonGlsl` is pasted into every stage. If paramsSize > 0, declare the
// matching block there (std140):
//   layout(set = 0, binding = 2) uniform Params { ... } params;
//
// Each stage then provides one function:
//   simulateGlsl: void particleSimulate(inout Particle p, uint id);
//   vertexGlsl:   void particleVertex(Particle p, uint id, vec2 corner,
//                                     out vec4 clipPos, out vec4 color,
//                                     out vec4 data);
//   fragmentGlsl: vec4 particleFragment(vec2 corner, vec4 color, vec4 data);
//   emissionGlsl: vec3 particleEmission(Particle p, uint id);   (optional)
//
// `corner` runs -1..1 across the sprite. particleFragment returns the blended
// colour directly, so PremultipliedAlpha effects must premultiply it.
//
// Two helpers are also available:
//   vertex:   float smokeTransmittance(vec3 world);
//     How much of this point survives the gas between it and the camera.
//     1 unless setDensityVolume was called.
//   fragment: float softDepthFade(float softness);
//     0 where this fragment touches scene geometry, 1 at `softness` metres clear.
struct GpuParticlesDesc {
    std::uint32_t count      = 0;
    ParticleBlend blend      = ParticleBlend::Additive;
    bool          depthTest  = true;  // false draws over the scene: lens-like glows.
    std::size_t   paramsSize = 0;
    std::string   commonGlsl;
    std::string   simulateGlsl;
    std::string   vertexGlsl;
    std::string   fragmentGlsl;
    std::string   emissionGlsl;  // Light this effect adds to a LightVolume.
    // Segments per ribbon. Above 1, each particle remembers where it has been
    // and draws as a continuous strip through those points instead of a sprite.
    // particleVertex still sets the look: data.x is the width.
    std::uint32_t trailSegments = 0;
    // Optional starting state: 16 floats per particle, the Particle struct in
    // order. Empty means every particle starts zeroed.
    std::vector<float> initialData;
};

class GpuParticles {
public:
    virtual ~GpuParticles() = default;

    // Uploads `paramsSize` bytes of Params, read by the next simulate/render.
    virtual void setParams(const void* params) = 0;

    // Advances one step by recording a compute dispatch into the current
    // frame. Call BETWEEN Renderer::beginFrame and endFrame; it can run several
    // times in a frame. Unlike ParticleSystem::tick it never stalls the GPU.
    virtual void simulate(float dtSec) = 0;

    // Draws into the HDR scene pass during endFrame, in call order, after the
    // opaque geometry. Call BETWEEN beginFrame and endFrame. Particles
    // depth-test against the scene (see GpuParticlesDesc::depthTest) but never
    // write depth.
    virtual void render(const Mat4& view, const Mat4& projection) = 0;

    // Adds each particle's emission to `volume`. Call between
    // LightVolume::beginInject and resolve. Does nothing when the effect has no
    // emissionGlsl.
    virtual void injectLight(LightVolume& volume) = 0;

    // Lets smokeTransmittance read `fluid`. `density` is extinction per unit of
    // fluid density, per metre: match VolumeRenderSettings::smokeDensity. Pass
    // nullptr to clear. Call before render, after the frame's fluid steps.
    virtual void setDensityVolume(const FluidVolume* fluid, float density) = 0;

    // Zeroes every particle on the next simulate().
    virtual void reset() = 0;

    virtual std::uint32_t count() const = 0;
};

// The complete GLSL for each stage of an effect. Exposed so tests can
// compile-check an effect without a GPU.
struct GpuParticleSources {
    std::string compute;
    std::string vertex;
    std::string fragment;
    std::string inject;    // Empty when the effect has no emissionGlsl.
};
GpuParticleSources composeGpuParticleSources(const GpuParticlesDesc& desc);

// Returns nullptr if the shaders fail to compile or the desc is invalid. Must
// be destroyed before the Renderer.
std::unique_ptr<GpuParticles> createGpuParticles(Renderer& renderer,
                                                 const GpuParticlesDesc& desc);

}  // namespace iron
