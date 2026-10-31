#pragma once

#include "render/backends/vulkan/VkComputePipeline.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace iron {

class VkContext;
class VulkanRenderer;

// One thing a compute dispatch reads or writes. A binding's type comes from the
// list given to VkComputeStage::init.
struct VkComputeResource {
    VkImageView   view    = VK_NULL_HANDLE;
    VkSampler     sampler = VK_NULL_HANDLE;
    VkImageLayout layout  = VK_IMAGE_LAYOUT_GENERAL;
    VkBuffer      buffer  = VK_NULL_HANDLE;
    VkDeviceSize  offset  = 0;
    VkDeviceSize  range   = VK_WHOLE_SIZE;

    static VkComputeResource image(VkImageView view) {
        VkComputeResource r;
        r.view = view;
        return r;
    }
    static VkComputeResource sampled(VkImageView view, VkSampler sampler,
                                     VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        VkComputeResource r;
        r.view = view;
        r.sampler = sampler;
        r.layout = layout;
        return r;
    }
    static VkComputeResource bufferRange(VkBuffer buffer, VkDeviceSize offset = 0,
                                         VkDeviceSize range = VK_WHOLE_SIZE) {
        VkComputeResource r;
        r.buffer = buffer;
        r.offset = offset;
        r.range = range;
        return r;
    }
};

// A compute shader plus the plumbing to dispatch it: its descriptor layout, and
// one descriptor set per dispatch from the frame's pool. Binding numbers in the
// GLSL follow the order of `bindings`.
class VkComputeStage {
public:
    bool init(VkContext& ctx, const std::string& glsl,
              std::initializer_list<VkDescriptorType> bindings,
              std::uint32_t pushConstantBytes = 0);
    void destroy(VkContext& ctx);

    void dispatch(VulkanRenderer& renderer, VkCommandBuffer cb,
                  std::initializer_list<VkComputeResource> resources, const void* push,
                  std::uint32_t groupsX, std::uint32_t groupsY = 1, std::uint32_t groupsZ = 1);

private:
    std::vector<VkDescriptorType> bindings_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkComputePipeline     pipeline_;
    std::uint32_t         pushBytes_ = 0;
};

// Makes earlier compute writes visible to later compute reads and writes.
void computeToComputeBarrier(VkCommandBuffer cb);

}  // namespace iron
