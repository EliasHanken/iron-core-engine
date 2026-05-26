#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "iron::ParticleSystem requires -DIRON_RENDER_BACKEND=vulkan"
#endif

#include "math/Mat4.h"
#include "math/Vec.h"

#include <cstdint>
#include <memory>

namespace iron {

class Renderer;  // forward

struct ParticleSystemConfig {
    std::uint32_t count        = 1'000'000;
    float         spawnRadius  = 20.0f;
    float         lifetimeMin  = 4.0f;
    float         lifetimeMax  = 8.0f;
    float         noiseScale   = 0.08f;
    float         noiseStrength = 4.0f;
    float         spriteSize   = 0.06f;
    Vec3          colorYoung   = {0.6f, 0.95f, 1.0f};   // bright cyan
    Vec3          colorOld     = {0.05f, 0.10f, 0.3f};  // deep blue
    std::uint32_t seed         = 0xC0FFEE;
};

// GPU-resident particle system. Construct AFTER Renderer is initialised.
// Vulkan-only — the header has an #error under any other backend.
class ParticleSystem {
public:
    virtual ~ParticleSystem() = default;

    // Advance one tick. dtSec is the simulation step (typically the
    // frame's delta time). Internally a single compute dispatch.
    virtual void tick(float dtSec) = 0;

    // Draw all live particles with the camera matrices for this frame.
    // Must be called BETWEEN Renderer::beginFrame and Renderer::endFrame.
    virtual void render(const Mat4& view, const Mat4& projection) = 0;

    virtual std::uint32_t count() const = 0;
};

// Factory. Returns nullptr if the Vulkan compute path failed to
// initialise. Caller owns the returned pointer.
std::unique_ptr<ParticleSystem> createParticleSystem(
    Renderer& renderer, const ParticleSystemConfig& cfg);

}  // namespace iron
