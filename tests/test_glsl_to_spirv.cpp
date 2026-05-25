// This test exercises the pure compileGlsl helper (no Vulkan context
// required). Only built under -DIRON_RENDER_BACKEND=vulkan.

#include "test_framework.h"

#ifdef IRON_RENDER_BACKEND_VULKAN

#include "render/backends/vulkan/VkShader.h"

#include <cstdint>
#include <string>

using namespace iron;

int main() {
    const std::string vert = R"(
        #version 450
        layout(location = 0) in vec3 aPos;
        void main() { gl_Position = vec4(aPos, 1.0); }
    )";
    const std::string frag = R"(
        #version 450
        layout(location = 0) out vec4 outColor;
        void main() { outColor = vec4(1.0); }
    )";

    // Vertex compile.
    {
        const auto spv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, vert);
        CHECK(!spv.empty());
        CHECK(spv.front() == 0x07230203u);  // SPIR-V magic
    }

    // Fragment compile.
    {
        const auto spv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, frag);
        CHECK(!spv.empty());
        CHECK(spv.front() == 0x07230203u);
    }

    return iron_test_result();
}

#else
int main() { return 0; }  // No-op when not building with Vulkan.
#endif
