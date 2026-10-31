#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "iron::VolumeRenderer requires -DIRON_RENDER_BACKEND=vulkan"
#endif

#include "math/Mat4.h"
#include "math/Vec.h"

#include <memory>

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
    float detail = 0.5f;           // Noise erosion at smoke edges, 0 for none.
    float detailScale = 3.0f;      // Noise frequency, per metre.
};

// Draws a LightVolume, and optionally a FluidVolume, as true volumetrics: a
// half-resolution raymarch that stops at scene depth, then a depth-aware upsample
// so it stays crisp against geometry. The result lands in the HDR scene before
// bloom, so it glows.
class VolumeRenderer {
public:
    virtual ~VolumeRenderer() = default;

    // Call between Renderer::beginFrame and endFrame, after any fluid steps and
    // before the frame's particle render calls, so that particles draw over it.
    // `fluid` may be null.
    virtual void render(const Mat4& view, const Mat4& projection, const LightVolume& light,
                        const FluidVolume* fluid, const VolumeRenderSettings& settings) = 0;
};

// Returns nullptr on failure. Must be destroyed before the Renderer.
std::unique_ptr<VolumeRenderer> createVolumeRenderer(Renderer& renderer);

}  // namespace iron
