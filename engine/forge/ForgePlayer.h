#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "iron::forge::Player requires -DIRON_RENDER_BACKEND=vulkan"
#endif

#include "forge/Forge.h"
#include "math/Mat4.h"
#include "render/GpuParticles.h"

#include <memory>
#include <string>
#include <vector>

namespace iron {
class FluidVolume;
class LightVolume;
class Renderer;
}

namespace iron::forge {

// The full GLSL for one layer, ready for iron::GpuParticles.
GpuParticlesDesc layerToDesc(const CompiledLayer& layer);

// Compiles every shader in `compiled`, turning failures into issues. Needs no GPU.
std::vector<Issue> checkShaders(const Compiled& compiled);

// Plays a compiled effect, and swaps in edits without ever going blank.
class Player {
public:
    explicit Player(Renderer& renderer) : renderer_(renderer) {}

    // Applies an edit. A change of values updates the running effect in place, and
    // a change of structure rebuilds it. If that fails, the last good effect keeps
    // running and the reasons are returned.
    std::vector<Issue> apply(const Compiled& compiled);

    // Call between Renderer::beginFrame and endFrame.
    void simulate(float dt);
    void injectLight(LightVolume& light);
    void render(const Mat4& view, const Mat4& projection);

    void setDensityVolume(const FluidVolume* fluid, float density);

    // Restarts the effect from nothing.
    void restart();

    std::uint32_t particleCount() const;

private:
    struct Running {
        CompiledLayer layer;
        std::unique_ptr<GpuParticles> effect;
    };

    Renderer& renderer_;
    std::vector<Running> layers_;
};

}  // namespace iron::forge
