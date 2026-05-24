#include "render/TextureLoader.h"

#include "core/Log.h"

#include <stb_image.h>

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

std::vector<unsigned char> loadRoughnessAsSpec(const std::string& path,
                                                int& outWidth, int& outHeight) {
    // stbi_set_flip_vertically_on_load matches the convention used by
    // GLTexture so the resulting texture aligns with diffuse/normal pairs.
    stbi_set_flip_vertically_on_load(1);
    int w = 0, h = 0, channels = 0;
    unsigned char* raw = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!raw) {
        Log::error("loadRoughnessAsSpec: failed to load '%s'", path.c_str());
        return {};
    }
    std::vector<unsigned char> pixels(raw, raw + (static_cast<std::size_t>(w) * h * 4));
    stbi_image_free(raw);
    invertRGBChannels(pixels);
    outWidth = w;
    outHeight = h;
    return pixels;
}

} // namespace iron
