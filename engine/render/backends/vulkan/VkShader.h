#pragma once

#include "render/Handles.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace iron {

class VkContext;

struct VkShader {
    VkShaderModule        vertexModule    = VK_NULL_HANDLE;
    VkShaderModule        fragmentModule  = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout       = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout  = VK_NULL_HANDLE;
};

// Compile a GLSL string for the given stage to SPIR-V. Returns an empty
// vector on failure. `stage` is VK_SHADER_STAGE_VERTEX_BIT or
// VK_SHADER_STAGE_FRAGMENT_BIT. Pure logic — no Vulkan calls — so it's
// unit-testable headlessly.
std::vector<std::uint32_t> compileGlsl(VkShaderStageFlagBits stage,
                                       const std::string& src);

class VkShaderStore {
public:
    ShaderHandle create(VkContext& ctx,
                        const std::string& vertSrc,
                        const std::string& fragSrc);
    const VkShader& get(ShaderHandle h) const;
    bool has(ShaderHandle h) const { return shaders_.count(h) != 0; }
    void destroyAll(VkContext& ctx);

private:
    std::unordered_map<ShaderHandle, VkShader> shaders_;
    ShaderHandle nextHandle_ = 1;
};

}  // namespace iron
