#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "iron::VolumeRenderer requires -DIRON_RENDER_BACKEND=vulkan"
#endif

#include "math/Mat4.h"
#include "math/Vec.h"

#include <memory>
#include <span>
#include <string>

namespace iron {

class FluidVolume;
class LightVolume;
class Renderer;

struct VolumeRenderSettings {
    float airDensity = 0.0f;       // Thin constant scattering across the light volume.
    int   steps = 56;              // March steps per half-resolution pixel.
    float lightIntensity = 1.0f;   // Multiplies the LightVolume.
    float smokeDensity = 1.0f;     // Extinction per unit of fluid density, per metre.
    Vec3  smokeAlbedo{0.20f, 0.12f, 0.28f};
    Vec3  heatColor{1.0f, 0.35f, 0.08f};
    float heatIntensity = 1.0f;
    float shadowDensity = 1.5f;    // How strongly smoke shades itself.
    Vec3  lightPosition{0.0f, 0.0f, 0.0f}; // The light smoke shades itself against.
    Vec3  lightColor{0.0f, 0.0f, 0.0f};    // Its colour and intensity, for direct light.
    float phase = 0.0f;            // Forward scattering: 0 is even, 0.8 glows toward the light.
    float powder = 0.0f;           // Darkens thin edges facing away from the light, 0..1.
    float airLight = 1.0f;         // Scales direct light in the air, which is what draws shafts.
    float detail = 0.5f;           // Noise erosion at smoke edges, 0 for none.
    float detailScale = 3.0f;      // Noise frequency, per metre.
    float time = 0.0f;             // Passed to the air hook.
};

// `airGlsl` is game GLSL that shapes the air. It's optional, and provides:
//   void airAt(vec3 p, float time, inout float density, inout vec3 light);
// `density` starts at airDensity. `light` starts at zero, and is light that's
// already in the air at p, such as shafts from a window, which needs no lamp.
struct VolumeRendererDesc {
    std::string airGlsl;
};

// Draws a LightVolume, and optionally a FluidVolume, as true volumetrics: a
// half-resolution raymarch that stops at scene depth, then a depth-aware upsample
// so it stays crisp against geometry. The result lands in the HDR scene before
// bloom, so it glows.
class VolumeRenderer {
public:
    virtual ~VolumeRenderer() = default;

    // Casts shadows from a light through the air, smoke and lit surfaces, which is
    // what draws god rays. Each sphere is xyz = centre, w = radius, and at most
    // kMaxOccluders are used. Each column is an endless vertical pillar, with x and z
    // as its position and y as its radius, which is sharper and far cheaper than a
    // stack of spheres. At most kMaxColumns are used. Call between beginFrame and
    // endFrame, before render. With nothing to cast them, shadows turn off.
    static constexpr int kMaxOccluders = 256;
    static constexpr int kMaxColumns = 64;
    virtual void setOccluders(Vec3 light, std::span<const Vec4> spheres, float strength,
                              std::span<const Vec3> columns = {}) = 0;

    // Call between Renderer::beginFrame and endFrame, after any fluid steps and
    // before the frame's particle render calls, so that particles draw over it.
    // `fluid` may be null.
    virtual void render(const Mat4& view, const Mat4& projection, const LightVolume& light,
                        const FluidVolume* fluid, const VolumeRenderSettings& settings) = 0;
};

// Returns nullptr on failure. Must be destroyed before the Renderer.
std::unique_ptr<VolumeRenderer> createVolumeRenderer(Renderer& renderer,
                                                     const VolumeRendererDesc& desc = {});

}  // namespace iron
