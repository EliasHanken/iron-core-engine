#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "iron::forge::Player requires -DIRON_RENDER_BACKEND=vulkan"
#endif

#include "forge/Forge.h"
#include "math/Mat4.h"
#include "render/GpuParticles.h"
#include "render/LightVolume.h"

#include <memory>
#include <string>
#include <vector>

namespace iron {
class FluidVolume;
class Renderer;
class VolumeRenderer;
}

namespace iron::forge {

// The full GLSL for one layer, ready for iron::GpuParticles.
GpuParticlesDesc layerToDesc(const CompiledLayer& layer);

// Compiles every shader in `compiled`, turning failures into issues. Needs no GPU.
std::vector<Issue> checkShaders(const Compiled& compiled);

// A dim studio: dark, with soft lights for metal to reflect.
const char* studioSkyGlsl();

// Plays a compiled effect, and swaps in edits without ever going blank.
class Player {
public:
    explicit Player(Renderer& renderer);
    ~Player();

    // Applies an edit. A change of values updates the running effect in place, and
    // a change of structure rebuilds it. If that fails, the last good effect keeps
    // running and the reasons are returned.
    std::vector<Issue> apply(const Compiled& compiled);

    // Call between Renderer::beginFrame and endFrame.
    void simulate(float dt);
    void render(const Mat4& view, const Mat4& projection);

    void setDensityVolume(const FluidVolume* fluid, float density);

    // Sets an Input or InputVector node by name, for this frame on. A name the
    // effect doesn't use is ignored, so a game can feed effects that skip some.
    void setInput(const std::string& name, Value value);

    // Where light gathers, and how visible it is in the air. Bounds apply when
    // the light is first needed, so set them before the first apply.
    void setLightVolume(const LightVolumeDesc& desc) { lightDesc_ = desc; }
    void setAirDensity(float density) { airDensity_ = density; }

    // Restarts the effect from nothing.
    void restart();

    std::uint32_t particleCount() const;

    // Set when a layer uses LightTheScene: the light on the air and on surfaces.
    const LightVolume* light() const { return light_.get(); }

private:
    struct Running {
        CompiledLayer layer;
        std::unique_ptr<GpuParticles> effect;
    };

    void updateLight();

    Renderer& renderer_;
    std::vector<Running> layers_;
    std::unique_ptr<LightVolume> light_;
    std::unique_ptr<VolumeRenderer> volumes_;
    LightVolumeDesc lightDesc_;
    float airDensity_ = 0.03f;
};

}  // namespace iron::forge
