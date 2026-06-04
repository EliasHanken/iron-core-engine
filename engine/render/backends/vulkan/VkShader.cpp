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
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    return EShLangTessControl;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return EShLangTessEvaluation;
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

    // M17/M45b/M45c/M46b/M46c/M50a — descriptor set layout: UBO + 8 samplers (diffuse, normal,
    // metallic-roughness, shadow, sky cubemap, planar reflection, AO, emissive) +
    // binding 10 = irradiance cubemap (M46b) + binding 11 = prefiltered specular cubemap +
    // binding 12 = BRDF integration LUT (M46c split-sum) +
    // binding 13 = height map (M50a POM; white fallback = depth 0 = POM no-op).
    VkDescriptorSetLayoutBinding bindings[13]{};
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
    bindings[3].binding = 3;  // metallic-roughness
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
    bindings[7].binding = 7;  // ambient occlusion (M45b)
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[8].binding = 8;  // emissive (M45c)
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[8].descriptorCount = 1;
    bindings[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[9].binding         = 10;   // M46b — irradiance cubemap (diffuse IBL)
    bindings[9].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[10].binding         = 11;   // M46c — prefiltered specular cubemap (split-sum)
    bindings[10].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[11].binding         = 12;   // M46c — BRDF integration LUT (split-sum)
    bindings[11].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[11].descriptorCount = 1;
    bindings[11].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[12].binding         = 13;   // M50a — height map (POM)
    bindings[12].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[12].descriptorCount = 1;
    bindings[12].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 13;
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

    // M23/M45b/M45c/M46b/M46c/M50a — 14 bindings: same 9 as the lit path (0=scene UBO, 1-8=samplers
    // including AO at 7 and emissive at 8) + binding 9 = bone-matrices UBO (vertex stage) +
    // binding 10 = irradiance cubemap (M46b) + binding 11 = prefiltered specular cubemap +
    // binding 12 = BRDF integration LUT (M46c split-sum) +
    // binding 13 = height map (M50a POM; white fallback = depth 0 = POM no-op).
    // Bones moved 7→8→9 to keep sampler bindings consistent with the shared frag shader.
    VkDescriptorSetLayoutBinding bindings[14]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int i = 1; i < 9; ++i) {
        bindings[i].binding = static_cast<std::uint32_t>(i);
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[9].binding = 9;  // bone matrices (M23, moved 7→8→9 as samplers grew)
    bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[10].binding         = 10;  // M46b — irradiance cubemap (diffuse IBL)
    bindings[10].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[11].binding         = 11;  // M46c — prefiltered specular cubemap (split-sum)
    bindings[11].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[11].descriptorCount = 1;
    bindings[11].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[12].binding         = 12;  // M46c — BRDF integration LUT (split-sum)
    bindings[12].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[12].descriptorCount = 1;
    bindings[12].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[13].binding         = 13;  // M50a — height map (POM)
    bindings[13].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[13].descriptorCount = 1;
    bindings[13].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 14;
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

ShaderHandle VkShaderStore::createTessellated(VkContext& ctx,
                                              const std::string& vertSrc,
                                              const std::string& tescSrc,
                                              const std::string& teseSrc,
                                              const std::string& fragSrc) {
    auto vspv    = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,                  vertSrc);
    auto tescspv = compileGlsl(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,    tescSrc);
    auto tesespv = compileGlsl(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, teseSrc);
    auto fspv    = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT,                fragSrc);
    if (vspv.empty() || tescspv.empty() || tesespv.empty() || fspv.empty()) {
        Log::error("VkShaderStore::createTessellated: shader compile failed");
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
    if (!makeModule(tescspv, s.tescModule)) {
        vkDestroyShaderModule(ctx.device(), s.vertexModule, nullptr);
        return kInvalidHandle;
    }
    if (!makeModule(tesespv, s.teseModule)) {
        vkDestroyShaderModule(ctx.device(), s.tescModule,   nullptr);
        vkDestroyShaderModule(ctx.device(), s.vertexModule, nullptr);
        return kInvalidHandle;
    }
    if (!makeModule(fspv, s.fragmentModule)) {
        vkDestroyShaderModule(ctx.device(), s.teseModule,   nullptr);
        vkDestroyShaderModule(ctx.device(), s.tescModule,   nullptr);
        vkDestroyShaderModule(ctx.device(), s.vertexModule, nullptr);
        return kInvalidHandle;
    }

    // M50b — same lit descriptor set layout as create(), with TESE stage ORed into
    // binding 0 (UBO) and binding 13 (height map) so the evaluation shader can read them.
    // M17/M45b/M45c/M46b/M46c/M50a — descriptor set layout: UBO + 8 samplers (diffuse, normal,
    // metallic-roughness, shadow, sky cubemap, planar reflection, AO, emissive) +
    // binding 10 = irradiance cubemap (M46b) + binding 11 = prefiltered specular cubemap +
    // binding 12 = BRDF integration LUT (M46c split-sum) +
    // binding 13 = height map (M50a POM; white fallback = depth 0 = POM no-op).
    VkDescriptorSetLayoutBinding bindings[13]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                           | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    bindings[1].binding = 1;  // diffuse
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;  // normal
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = 3;  // metallic-roughness
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
    bindings[7].binding = 7;  // ambient occlusion (M45b)
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[8].binding = 8;  // emissive (M45c)
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[8].descriptorCount = 1;
    bindings[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[9].binding         = 10;   // M46b — irradiance cubemap (diffuse IBL)
    bindings[9].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[10].binding         = 11;   // M46c — prefiltered specular cubemap (split-sum)
    bindings[10].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[11].binding         = 12;   // M46c — BRDF integration LUT (split-sum)
    bindings[11].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[11].descriptorCount = 1;
    bindings[11].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[12].binding         = 13;   // M50a — height map (POM); M50b — also read by tese
    bindings[12].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[12].descriptorCount = 1;
    bindings[12].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT
                                 | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 13;
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
        if (s.teseModule)     vkDestroyShaderModule(ctx.device(), s.teseModule,     nullptr);  // M50b
        if (s.tescModule)     vkDestroyShaderModule(ctx.device(), s.tescModule,     nullptr);  // M50b
        if (s.vertexModule)   vkDestroyShaderModule(ctx.device(), s.vertexModule, nullptr);
    }
    shaders_.clear();
}

}  // namespace iron
