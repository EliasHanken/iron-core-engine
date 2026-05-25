// VkComputePipeline.cpp — SPIR-V → compute pipeline + matching layout.

#include "render/backends/vulkan/VkComputePipeline.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

namespace iron {

bool VkComputePipeline::init(VkContext& ctx,
                             const std::vector<std::uint32_t>& spirv,
                             VkDescriptorSetLayout setLayout) {
    VkShaderModuleCreateInfo modInfo{};
    modInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    modInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
    modInfo.pCode = spirv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &modInfo, nullptr, &module_));
    if (module_ == VK_NULL_HANDLE) return false;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &setLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &layout_));
    if (layout_ == VK_NULL_HANDLE) return false;

    VkComputePipelineCreateInfo cpInfo{};
    cpInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpInfo.stage.module = module_;
    cpInfo.stage.pName = "main";
    cpInfo.layout = layout_;

    VK_CHECK(vkCreateComputePipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                       &cpInfo, nullptr, &pipeline_));
    return pipeline_ != VK_NULL_HANDLE;
}

void VkComputePipeline::destroy(VkContext& ctx) {
    if (pipeline_) { vkDestroyPipeline(ctx.device(), pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_)   { vkDestroyPipelineLayout(ctx.device(), layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (module_)   { vkDestroyShaderModule(ctx.device(), module_, nullptr); module_ = VK_NULL_HANDLE; }
}

}  // namespace iron
