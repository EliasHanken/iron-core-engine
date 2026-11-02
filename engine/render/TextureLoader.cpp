#include "render/TextureLoader.h"

#include "render/Renderer.h"
#include "core/Log.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>

namespace iron {

void invertRGBChannels(std::vector<unsigned char>& pixels) {
    const std::size_t fullPixels = pixels.size() / 4;
    for (std::size_t i = 0; i < fullPixels; ++i) {
        const std::size_t base = i * 4;
        pixels[base + 0] = static_cast<unsigned char>(255 - pixels[base + 0]);
        pixels[base + 1] = static_cast<unsigned char>(255 - pixels[base + 1]);
        pixels[base + 2] = static_cast<unsigned char>(255 - pixels[base + 2]);
        // Alpha (pixels[base + 3]) intentionally left as-is.
    }
}

TextureHandle loadMetallicRoughness(Renderer& r, const std::string& roughnessPath,
                                    float metallicConstant) {
    // The rows must come out the way this backend's own loadTexture lays them, or
    // roughness ends up mirrored against the albedo. stb's flip setting is global to
    // the process, so it's set for this load and put back to top-first after.
    stbi_set_flip_vertically_on_load(r.texturesBottomUp() ? 1 : 0);
    int w = 0, h = 0, channels = 0;
    unsigned char* raw = stbi_load(roughnessPath.c_str(), &w, &h, &channels, 4);
    stbi_set_flip_vertically_on_load(0);
    if (!raw) {
        Log::error("loadMetallicRoughness: failed to load '%s'", roughnessPath.c_str());
        return kInvalidHandle;
    }

    const auto metallicByte = static_cast<unsigned char>(
        std::round(std::clamp(metallicConstant, 0.0f, 1.0f) * 255.0f));

    const std::size_t pixelCount = static_cast<std::size_t>(w) * h;
    std::vector<unsigned char> rgba(pixelCount * 4);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        const unsigned char roughnessVal = raw[i * 4 + 0];  // red channel = luminance
        rgba[i * 4 + 0] = 255;            // R — unused; set to 1 (glTF convention)
        rgba[i * 4 + 1] = roughnessVal;   // G = roughness
        rgba[i * 4 + 2] = metallicByte;   // B = metallic
        rgba[i * 4 + 3] = 255;            // A — unused
    }
    stbi_image_free(raw);

    return r.createTexture(w, h, rgba.data(), /*srgb=*/false);
}

} // namespace iron
