// Unit tests for the SSAO kernel + range-check CPU port (Ssao.h), kept in lockstep
// with the SSAO GLSL. Also compile-checks the SSAO + blur shaders (Vulkan only).
#include "render/Ssao.h"

#include <cassert>
#include <cmath>
#include <cstdio>

#ifdef IRON_RENDER_BACKEND_VULKAN
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkPostProcess.h"
#endif

using iron::Vec3;
using iron::generateSsaoKernel;
using iron::ssaoSampleOcclusion;

static bool approx(float a, float b, float eps = 1e-3f) {
    float d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

int main() {
    // Kernel: all samples in the unit hemisphere (z >= 0, |v| <= 1).
    {
        auto k = generateSsaoKernel(32);
        assert(k.size() == 32);
        for (const Vec3& s : k) {
            assert(s.z >= 0.0f);
            float len = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
            assert(len <= 1.0f + 1e-4f);
        }
        // Origin-weighted: the first few samples sit closer to the origin than the last few.
        float earlyLen = 0.0f, lateLen = 0.0f;
        for (int i = 0; i < 4; ++i) {
            const Vec3& e = k[i];        earlyLen += std::sqrt(e.x*e.x + e.y*e.y + e.z*e.z);
            const Vec3& l = k[28 + i];   lateLen  += std::sqrt(l.x*l.x + l.y*l.y + l.z*l.z);
        }
        assert(earlyLen < lateLen);
    }
    // Determinism: same seed → identical kernel.
    {
        auto a = generateSsaoKernel(16);
        auto b = generateSsaoKernel(16);
        for (size_t i = 0; i < a.size(); ++i)
            assert(approx(a[i].x, b[i].x) && approx(a[i].y, b[i].y) && approx(a[i].z, b[i].z));
    }
    // Range-check / occlusion helper endpoints.
    {
        // Not occluded: the surface behind the sample point → 0.
        assert(approx(ssaoSampleOcclusion(/*fragZ*/-1.0f, /*sampleZ*/-2.0f, /*surfaceZ*/-2.5f, 0.5f, 0.025f), 0.0f));
        // Occluded and within range: surface closer than the sample → > 0.
        float occ = ssaoSampleOcclusion(-1.0f, -2.0f, -1.5f, 0.5f, 0.025f);
        assert(occ > 0.0f && occ <= 1.0f);
        // Occluded but far out of range → contribution falls off toward 0.
        float far = ssaoSampleOcclusion(-1.0f, -2.0f, 5.0f, 0.5f, 0.025f);
        assert(far >= 0.0f && far < occ);
    }

#ifdef IRON_RENDER_BACKEND_VULKAN
    {  // SSAO + blur shaders compile to SPIR-V.
        const char* srcs[] = { iron::kSsaoSrc(), iron::kSsaoBlurSrc() };
        for (const char* src : srcs) {
            const auto spv = iron::compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, src);
            assert(!spv.empty());
            assert(spv.front() == 0x07230203u);
        }
    }
#endif

    std::puts("test_ssao: OK");
    return 0;
}
