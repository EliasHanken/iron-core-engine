#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "iron::LightVolume requires -DIRON_RENDER_BACKEND=vulkan"
#endif

#include "render/VolumeBounds.h"

#include <memory>

namespace iron {

class Renderer;

struct LightVolumeDesc {
    VolumeBounds bounds;
    VolumeSize   size{128, 128, 128};
    int          blurPasses = 2;
};

// A 3D grid of light filled from particle emission, so that an effect lights
// its surroundings without anyone placing a light. Each frame, between
// beginFrame and endFrame:
//   volume->beginInject();
//   effect->injectLight(*volume);   // Any number of effects.
//   volume->resolve();
// Sample the result with VulkanRenderer::setLightVolume or VolumeRenderer.
class LightVolume {
public:
    virtual ~LightVolume() = default;

    virtual void beginInject() = 0;
    virtual void resolve() = 0;

    virtual const VolumeBounds& bounds() const = 0;
    virtual VolumeSize size() const = 0;
};

// Returns nullptr on failure. Must be destroyed before the Renderer.
std::unique_ptr<LightVolume> createLightVolume(Renderer& renderer, const LightVolumeDesc& desc);

}  // namespace iron
