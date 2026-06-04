// Guards the engine-owned standard lit shader sources against syntax rot:
// each must compile to non-empty SPIR-V via the same glslang path the engine uses.

#include "test_framework.h"

#ifdef IRON_RENDER_BACKEND_VULKAN

#include "render/StandardLitShader.h"
#include "render/backends/vulkan/VkShader.h"

#include <cstdint>
#include <string>

using namespace iron;

int main() {
    const auto vert    = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   standardLitVertSource());
    const auto skinned = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   standardSkinnedLitVertSource());
    const auto frag    = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT,  standardLitFragSource());

    CHECK(!vert.empty());
    CHECK(!skinned.empty());
    CHECK(!frag.empty());

    // M50b — tessellation sources.
    const auto tessVert = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,
                                      standardLitTessVertSource());
    const auto tesc     = compileGlsl(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                      standardLitTescSource());
    const auto tese     = compileGlsl(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                      standardLitTeseSource());
    CHECK(!tessVert.empty());
    CHECK(!tesc.empty());
    CHECK(!tese.empty());

    return iron_test_result();
}

#else
int main() { return 0; }  // No-op when not building with Vulkan.
#endif
