#include "render/backends/vulkan/VkComputeStage.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include "core/Log.h"

#include <cassert>

namespace iron {

bool VkComputeStage::init(VkContext& ctx, const std::string& glsl,
                          std::initializer_list<VkDescriptorType> bindings,
                          std::uint32_t pushConstantBytes) {
    bindings_ = bindings;
    pushBytes_ = pushConstantBytes;

    const auto spirv = compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, glsl);
    if (spirv.empty()) {
        Log::error("VkComputeStage: compute shader compile failed");
        return false;
    }

    std::vector<VkDescriptorSetLayoutBinding> layout(bindings_.size());
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        layout[i].binding         = static_cast<std::uint32_t>(i);
        layout[i].descriptorType  = bindings_[i];
        layout[i].descriptorCount = 1;
        layout[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<std::uint32_t>(layout.size());
    info.pBindings    = layout.data();
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &info, nullptr, &setLayout_));

    return pipeline_.init(ctx, spirv, setLayout_, pushConstantBytes);
}

void VkComputeStage::destroy(VkContext& ctx) {
    pipeline_.destroy(ctx);
    if (setLayout_) vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr);
    setLayout_ = VK_NULL_HANDLE;
}

void VkComputeStage::dispatch(VulkanRenderer& renderer, VkCommandBuffer cb,
                              std::initializer_list<VkComputeResource> resources,
                              const void* push, std::uint32_t groupsX, std::uint32_t groupsY,
                              std::uint32_t groupsZ) {
    assert(resources.size() == bindings_.size());

    VkContext& ctx = renderer.context();
    VkFrameRing::Frame& frame = renderer.frameRing().current();

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = frame.descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &setLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &allocInfo, &set));

    const std::size_t count = bindings_.size();
    std::vector<VkDescriptorImageInfo>  images(count);
    std::vector<VkDescriptorBufferInfo> buffers(count);
    std::vector<VkWriteDescriptorSet>   writes(count);

    std::size_t i = 0;
    for (const VkComputeResource& r : resources) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = set;
        writes[i].dstBinding      = static_cast<std::uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = bindings_[i];
        if (r.buffer != VK_NULL_HANDLE) {
            buffers[i] = {r.buffer, r.offset, r.range};
            writes[i].pBufferInfo = &buffers[i];
        } else {
            images[i] = {r.sampler, r.view, r.layout};
            writes[i].pImageInfo = &images[i];
        }
        ++i;
    }
    vkUpdateDescriptorSets(ctx.device(), static_cast<std::uint32_t>(count), writes.data(), 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.pipelineLayout(), 0, 1,
                            &set, 0, nullptr);
    if (pushBytes_ > 0) {
        vkCmdPushConstants(cb, pipeline_.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           pushBytes_, push);
    }
    vkCmdDispatch(cb, groupsX, groupsY, groupsZ);
}

void computeToComputeBarrier(VkCommandBuffer cb) {
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

}  // namespace iron
