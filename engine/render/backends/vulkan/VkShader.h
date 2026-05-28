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
    // M23 — like create(), but builds an 8-binding descriptor set layout
    // (the 7 scene bindings + binding 7 = vertex-stage UBO of bone matrices).
    ShaderHandle createSkinned(VkContext& ctx,
                                const std::string& vertSrc,
                                const std::string& fragSrc);
    const VkShader& get(ShaderHandle h) const;
    bool has(ShaderHandle h) const { return shaders_.count(h) != 0; }

    // M28 — recompile the GLSL for an existing handle and swap the shader
    // modules in place. Keeps the same descriptor-set + pipeline layout
    // (interface assumed unchanged). Returns false (and leaves the old
    // modules intact) if either stage fails to compile. The VkShader's
    // address is stable, so the caller invalidates the cached pipeline
    // separately via VkPipeline::invalidate(&get(handle)).
    bool reload(VkContext& ctx, ShaderHandle h,
                const std::string& vertSrc, const std::string& fragSrc);

    void destroyAll(VkContext& ctx);

private:
    std::unordered_map<ShaderHandle, VkShader> shaders_;
    ShaderHandle nextHandle_ = 1;
};

}  // namespace iron
