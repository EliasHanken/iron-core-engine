#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkHud is Vulkan-only."
#endif

#include "render/HudBatch.h"
#include "render/Handles.h"

#include <vulkan/vulkan.h>

namespace iron {

class VkContext;
class VkFrameRing;
class VkTextureStore;

// Vulkan screen-space HUD renderer. record() iterates each HudDrawGroup,
// allocates a descriptor set from the active frame pool, writes the
// screen-size UBO + texture binding, sub-allocates vertices, and draws.
class VkHud {
public:
    bool init(VkContext& ctx, VkRenderPass scenePass);
    void destroy(VkContext& ctx);

    void record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                VkTextureStore& textures,
                const HudBatch& batch, int fbW, int fbH);

private:
    struct ScreenUbo {
        float screenSize[4];  // x, y, _, _ (std140 vec2 → 16 bytes)
    };

    bool ok_ = false;
    VkDescriptorSetLayout setLayout_      = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          pipeline_       = VK_NULL_HANDLE;  // qualified to avoid iron::VkPipeline shadow
};

}  // namespace iron
