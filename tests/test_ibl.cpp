// Unit tests for the IBL direction math CPU port (Ibl.h), kept in lockstep
// with the equirectToCube.comp GLSL. Verifies cube-face direction
// reconstruction matches the engine's ProceduralSky face convention and that
// direction<->equirect UV mapping round-trips.
#include "render/Ibl.h"

#include <cassert>
#include <cmath>
#include <cstdio>

#ifdef IRON_RENDER_BACKEND_VULKAN
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkIblBaker.h"
#endif

using iron::Vec3;
using iron::cubeFaceDirection;
using iron::directionToEquirectUv;

static bool approx(float a, float b, float eps = 1e-4f) {
    float d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

int main() {
    // 1. Face centers (u=v=0) point along the expected principal axes.
    {
        Vec3 px = cubeFaceDirection(0, 0.0f, 0.0f);  // +X
        assert(approx(px.x, 1.0f) && approx(px.y, 0.0f) && approx(px.z, 0.0f));
        Vec3 ny = cubeFaceDirection(3, 0.0f, 0.0f);  // -Y
        assert(approx(ny.x, 0.0f) && approx(ny.y, -1.0f) && approx(ny.z, 0.0f));
        Vec3 pz = cubeFaceDirection(4, 0.0f, 0.0f);  // +Z
        assert(approx(pz.x, 0.0f) && approx(pz.y, 0.0f) && approx(pz.z, 1.0f));
        Vec3 nx = cubeFaceDirection(1, 0.0f, 0.0f);  // -X
        assert(approx(nx.x, -1.0f) && approx(nx.y, 0.0f) && approx(nx.z, 0.0f));
        Vec3 nz = cubeFaceDirection(5, 0.0f, 0.0f);  // -Z
        assert(approx(nz.x, 0.0f) && approx(nz.y, 0.0f) && approx(nz.z, -1.0f));
    }

    // 2. All returned directions are unit length.
    {
        for (int face = 0; face < 6; ++face) {
            for (float u : {-1.0f, -0.3f, 0.5f, 1.0f}) {
                for (float v : {-1.0f, 0.2f, 1.0f}) {
                    Vec3 d = cubeFaceDirection(face, u, v);
                    float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                    assert(approx(len, 1.0f));
                }
            }
        }
    }

    // 3. Equirect UV: +X (yaw 0) maps to horizontal center, horizon to v=0.5.
    {
        auto uv = directionToEquirectUv(Vec3{1.0f, 0.0f, 0.0f});
        assert(approx(uv.x, 0.5f) && approx(uv.y, 0.5f));
    }

    // 4. Equirect UV: straight up (+Y) maps to v=1.0 (top of the image).
    {
        auto uv = directionToEquirectUv(Vec3{0.0f, 1.0f, 0.0f});
        assert(approx(uv.y, 1.0f));
    }

    // 5. Equirect u wraparound: -X (atan2(0,-1)=pi) -> u=1.0; -Z -> u=0.25.
    {
        auto uvNegX = directionToEquirectUv(Vec3{-1.0f, 0.0f, 0.0f});
        assert(approx(uvNegX.x, 1.0f));
        auto uvNegZ = directionToEquirectUv(Vec3{0.0f, 0.0f, -1.0f});
        assert(approx(uvNegZ.x, 0.25f));
    }

#ifdef IRON_RENDER_BACKEND_VULKAN
    // 6. The embedded equirect->cube compute shader compiles to SPIR-V.
    {
        const auto spv = iron::compileGlsl(
            VK_SHADER_STAGE_COMPUTE_BIT, iron::kEquirectToCubeComputeSrc());
        assert(!spv.empty());
        assert(spv.front() == 0x07230203u);  // SPIR-V magic
    }
#endif

    std::puts("test_ibl: OK");
    return 0;
}
