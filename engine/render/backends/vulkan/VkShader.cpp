// VkShader.cpp — glslang-based GLSL→SPIR-V compile + descriptor set
// layout for the spinning-cube binding contract.

#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace iron {

namespace {

struct GlslangInit {
    GlslangInit()  { glslang::InitializeProcess(); }
    ~GlslangInit() { glslang::FinalizeProcess(); }
};

void ensureGlslangInit() {
    static GlslangInit init;
    (void)init;
}

EShLanguage toLang(VkShaderStageFlagBits stage) {
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:   return EShLangVertex;
        case VK_SHADER_STAGE_FRAGMENT_BIT: return EShLangFragment;
        case VK_SHADER_STAGE_COMPUTE_BIT:  return EShLangCompute;
        default:                            return EShLangVertex;
    }
}

}  // namespace

std::vector<std::uint32_t> compileGlsl(VkShaderStageFlagBits stage,
                                       const std::string& src) {
    ensureGlslangInit();

    const EShLanguage lang = toLang(stage);
    glslang::TShader shader(lang);
    const char* srcs[] = { src.c_str() };
    shader.setStrings(srcs, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    const TBuiltInResource* resources = GetDefaultResources();
    const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.parse(resources, 450, false, messages)) {
        Log::error("compileGlsl: parse failed:\n%s", shader.getInfoLog());
        return {};
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        Log::error("compileGlsl: link failed:\n%s", program.getInfoLog());
        return {};
    }

    std::vector<std::uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv);
    return spirv;
}

ShaderHandle VkShaderStore::create(VkContext& ctx,
                                   const std::string& vertSrc,
                                   const std::string& fragSrc) {
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, vertSrc);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, fragSrc);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkShaderStore: shader compile failed");
        return kInvalidHandle;
    }

    VkShader s{};

    auto makeModule = [&](const std::vector<std::uint32_t>& code, VkShaderModule& out) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        VK_CHECK(vkCreateShaderModule(ctx.device(), &info, nullptr, &out));
        return out != VK_NULL_HANDLE;
    };
    if (!makeModule(vspv, s.vertexModule))   return kInvalidHandle;
    if (!makeModule(fspv, s.fragmentModule)) {
        vkDestroyShaderModule(ctx.device(), s.vertexModule, nullptr);
        return kInvalidHandle;
    }

    // M17 — descriptor set layout: UBO + 6 samplers (diffuse, normal, spec, shadow, sky cubemap, planar reflection).
    VkDescriptorSetLayoutBinding bindings[7]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;  // diffuse
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;  // normal
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = 3;  // spec
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[4].binding = 4;  // shadow
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[5].binding = 5;  // sky cubemap (M16)
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[6].binding = 6;  // planar reflection RTT (M17)
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 7;
    dslInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &s.setLayout));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &s.setLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &s.pipelineLayout));

    const ShaderHandle h = nextHandle_++;
    shaders_[h] = s;
    return h;
}

const VkShader& VkShaderStore::get(ShaderHandle h) const {
    auto it = shaders_.find(h);
    return it->second;  // caller-checked via has()
}

ShaderHandle VkShaderStore::createSkinned(VkContext& ctx,
                                           const std::string& vertSrc,
                                           const std::string& fragSrc) {
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, vertSrc);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, fragSrc);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkShaderStore::createSkinned: shader compile failed");
        return kInvalidHandle;
    }

    VkShader s{};

    auto makeModule = [&](const std::vector<std::uint32_t>& code, VkShaderModule& out) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        VK_CHECK(vkCreateShaderModule(ctx.device(), &info, nullptr, &out));
        return out != VK_NULL_HANDLE;
    };
    if (!makeModule(vspv, s.vertexModule))   return kInvalidHandle;
    if (!makeModule(fspv, s.fragmentModule)) {
        vkDestroyShaderModule(ctx.device(), s.vertexModule, nullptr);
        return kInvalidHandle;
    }

    // M23 — 8 bindings: same 7 as the scene path + binding 7 = bone-matrices
    // UBO (vertex stage). Allows the skinned vertex shader to read up to
    // kMaxBonesPerSkinnedMesh mat4 transforms per draw.
    VkDescriptorSetLayoutBinding bindings[8]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int i = 1; i < 7; ++i) {
        bindings[i].binding = static_cast<std::uint32_t>(i);
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[7].binding = 7;  // bone matrices (M23)
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 8;
    dslInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &s.setLayout));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &s.setLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &s.pipelineLayout));

    const ShaderHandle h = nextHandle_++;
    shaders_[h] = s;
    return h;
}

bool VkShaderStore::reload(VkContext& ctx, ShaderHandle h,
                           const std::string& vertSrc, const std::string& fragSrc) {
    auto it = shaders_.find(h);
    if (it == shaders_.end()) {
        Log::error("VkShaderStore::reload: unknown handle %u", h);
        return false;
    }

    // Compile first; if either stage fails, keep the old modules untouched.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, vertSrc);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, fragSrc);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkShaderStore::reload: compile failed; keeping last-good shader");
        return false;
    }

    auto makeModule = [&](const std::vector<std::uint32_t>& code) -> VkShaderModule {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        VkShaderModule m = VK_NULL_HANDLE;
        VK_CHECK(vkCreateShaderModule(ctx.device(), &info, nullptr, &m));
        return m;
    };
    VkShaderModule newVert = makeModule(vspv);
    VkShaderModule newFrag = makeModule(fspv);
    if (newVert == VK_NULL_HANDLE || newFrag == VK_NULL_HANDLE) {
        if (newVert) vkDestroyShaderModule(ctx.device(), newVert, nullptr);
        if (newFrag) vkDestroyShaderModule(ctx.device(), newFrag, nullptr);
        return false;
    }

    // Swap in place: destroy old modules, keep setLayout + pipelineLayout.
    VkShader& s = it->second;
    if (s.vertexModule)   vkDestroyShaderModule(ctx.device(), s.vertexModule, nullptr);
    if (s.fragmentModule) vkDestroyShaderModule(ctx.device(), s.fragmentModule, nullptr);
    s.vertexModule   = newVert;
    s.fragmentModule = newFrag;
    return true;
}

void VkShaderStore::destroyAll(VkContext& ctx) {
    for (auto& [h, s] : shaders_) {
        if (s.pipelineLayout) vkDestroyPipelineLayout(ctx.device(), s.pipelineLayout, nullptr);
        if (s.setLayout)      vkDestroyDescriptorSetLayout(ctx.device(), s.setLayout, nullptr);
        if (s.fragmentModule) vkDestroyShaderModule(ctx.device(), s.fragmentModule, nullptr);
        if (s.vertexModule)   vkDestroyShaderModule(ctx.device(), s.vertexModule, nullptr);
    }
    shaders_.clear();
}

}  // namespace iron
