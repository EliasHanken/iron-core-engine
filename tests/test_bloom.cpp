// Unit tests for the bloom prefilter math CPU port (Bloom.h), kept in lockstep
// with the prefilter()/karis() GLSL in VkPostProcess.cpp. Also compile-checks the
// bloom fragment shaders (Vulkan backend only).
#include "render/Bloom.h"

#include <cassert>
#include <cstdio>

#ifdef IRON_RENDER_BACKEND_VULKAN
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkPostProcess.h"
#endif

using iron::Vec3;
using iron::bloomPrefilter;
using iron::karisWeight;

static bool approx(float a, float b, float eps = 1e-3f) {
    float d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

int main() {
    const float thr = 1.0f, knee = 0.5f;

    // Below (threshold - knee): no bloom contribution.
    {
        Vec3 r = bloomPrefilter(Vec3{0.2f, 0.2f, 0.2f}, thr, knee);
        assert(approx(r.x, 0.0f) && approx(r.y, 0.0f) && approx(r.z, 0.0f));
    }
    // Far above threshold: approaches passthrough scaled by (br-thr)/br.
    // br=5, thr=1 -> contrib ~= 4/5 -> 5*0.8 = 4.0 per channel.
    {
        Vec3 r = bloomPrefilter(Vec3{5.0f, 5.0f, 5.0f}, thr, knee);
        assert(approx(r.x, 4.0f, 0.05f));
    }
    // Monotonic non-decreasing across the knee, always finite/non-negative.
    {
        float prev = -1.0f;
        for (int i = 0; i <= 20; ++i) {
            float b = 0.5f + i * 0.1f;
            Vec3 r = bloomPrefilter(Vec3{b, b, b}, thr, knee);
            assert(r.x >= prev - 1e-4f);
            assert(r.x >= 0.0f);
            prev = r.x;
        }
    }
    // Karis weight: brighter samples get less weight; weight in (0, 1].
    {
        float wDark   = karisWeight(Vec3{0.0f, 0.0f, 0.0f});
        float wBright = karisWeight(Vec3{4.0f, 4.0f, 4.0f});
        assert(approx(wDark, 1.0f));
        assert(wBright > 0.0f && wBright < wDark);
    }

#ifdef IRON_RENDER_BACKEND_VULKAN
    {  // Bloom fragment shaders compile to SPIR-V.
        const char* srcs[] = {
            iron::kBloomPrefilterDownSrc(),
            iron::kBloomDownsampleSrc(),
            iron::kBloomUpsampleSrc(),
        };
        for (const char* src : srcs) {
            const auto spv = iron::compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, src);
            assert(!spv.empty());
            assert(spv.front() == 0x07230203u);
        }
    }
#endif

    std::puts("test_bloom: OK");
    return 0;
}
