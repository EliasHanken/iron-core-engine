#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace iron {

class VkContext;

// Private Vulkan-backend helper: SPIR-V → compute pipeline + matching
// pipeline layout. Owns nothing outside its own pipeline/layout/module
// handles; caller supplies the descriptor set layout.
class VkComputePipeline {
public:
    bool init(VkContext& ctx,
              const std::vector<std::uint32_t>& spirv,
              VkDescriptorSetLayout setLayout);
    void destroy(VkContext& ctx);

    ::VkPipeline     pipeline()       const { return pipeline_; }
    VkPipelineLayout pipelineLayout() const { return layout_; }

private:
    VkShaderModule   module_   = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
    ::VkPipeline     pipeline_ = VK_NULL_HANDLE;
};

}  // namespace iron
