#include "render/ProceduralSky.h"

#include "math/Vec.h"
#include "render/Renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace iron {

namespace {

// Fills one cubemap face (RGBA8) with the sunset gradient. `face` is the cube
// face index in the engine's order: +X, -X, +Y, -Y, +Z, -Z. The per-texel sky
// direction is reconstructed from the face + (u, v), normalized, and shaded by
// its y component: orange at the horizon, fading up to magenta then deep blue,
// and down to a dark ground.
void generateSunsetFace(int face, int faceSize, std::vector<unsigned char>& pixels) {
    pixels.assign(static_cast<std::size_t>(faceSize) * faceSize * 4, 0);

    const Vec3 cZenith{0.10f, 0.18f, 0.40f};
    const Vec3 cMid   {0.85f, 0.45f, 0.45f};
    const Vec3 cHoriz {1.00f, 0.55f, 0.30f};
    const Vec3 cGround{0.20f, 0.12f, 0.10f};

    for (int y = 0; y < faceSize; ++y) {
        for (int x = 0; x < faceSize; ++x) {
            const float u = 2.0f * (x + 0.5f) / faceSize - 1.0f;
            const float v = 2.0f * (y + 0.5f) / faceSize - 1.0f;
            Vec3 dir{};
            switch (face) {
                case 0: dir = { 1.0f, -v,   -u};   break;
                case 1: dir = {-1.0f, -v,    u};   break;
                case 2: dir = { u,    1.0f,  v};   break;
                case 3: dir = { u,   -1.0f, -v};   break;
                case 4: dir = { u,   -v,    1.0f}; break;
                case 5: dir = {-u,   -v,   -1.0f}; break;
            }
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            dir = {dir.x / len, dir.y / len, dir.z / len};

            const float skyY = dir.y;
            Vec3 color;
            if (skyY >= 0.0f) {
                const float t = skyY;
                const float horizMid = std::min(t * 2.0f, 1.0f);
                const float midZen   = std::max((t - 0.5f) * 2.0f, 0.0f);
                const Vec3 a = {
                    cHoriz.x + (cMid.x - cHoriz.x) * horizMid,
                    cHoriz.y + (cMid.y - cHoriz.y) * horizMid,
                    cHoriz.z + (cMid.z - cHoriz.z) * horizMid,
                };
                color = {
                    a.x + (cZenith.x - a.x) * midZen,
                    a.y + (cZenith.y - a.y) * midZen,
                    a.z + (cZenith.z - a.z) * midZen,
                };
            } else {
                const float t = -skyY;
                color = {
                    cHoriz.x + (cGround.x - cHoriz.x) * t,
                    cHoriz.y + (cGround.y - cHoriz.y) * t,
                    cHoriz.z + (cGround.z - cHoriz.z) * t,
                };
            }

            const int idx = (y * faceSize + x) * 4;
            pixels[idx + 0] = static_cast<unsigned char>(std::clamp(color.x * 255.0f, 0.0f, 255.0f));
            pixels[idx + 1] = static_cast<unsigned char>(std::clamp(color.y * 255.0f, 0.0f, 255.0f));
            pixels[idx + 2] = static_cast<unsigned char>(std::clamp(color.z * 255.0f, 0.0f, 255.0f));
            pixels[idx + 3] = 255;
        }
    }
}

}  // namespace

CubemapHandle createSunsetSkybox(Renderer& renderer, int faceSize) {
    if (faceSize <= 0) return kInvalidHandle;

    std::vector<unsigned char> faceData[6];
    std::array<const unsigned char*, 6> facePtrs{};
    for (int i = 0; i < 6; ++i) {
        generateSunsetFace(i, faceSize, faceData[i]);
        facePtrs[i] = faceData[i].data();
    }
    return renderer.createCubemap(faceSize, faceSize, facePtrs);
}

}  // namespace iron
