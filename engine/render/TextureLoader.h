#pragma once

#include <string>
#include <vector>

namespace iron {

// Inverts the R, G, B channels of an RGBA byte buffer in place
// (channel = 255 - channel). Alpha is left untouched. Use this to convert a
// Polyhaven-style roughness map (1 = matte) into a specular-intensity map
// (1 = shiny) without touching the shader.
//
// `pixels.size()` should be a multiple of 4; trailing bytes (if any) are
// ignored. An empty buffer is a no-op.
void invertRGBChannels(std::vector<unsigned char>& pixels);

// Loads a PNG/JPG from `path` via stb_image as RGBA, then inverts R/G/B per
// `invertRGBChannels`. Writes the image's pixel dimensions into `outWidth`
// and `outHeight`. Returns an empty vector on failure (and leaves the out
// params untouched).
std::vector<unsigned char> loadRoughnessAsSpec(const std::string& path,
                                                int& outWidth, int& outHeight);

} // namespace iron
