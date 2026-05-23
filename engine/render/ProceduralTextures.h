#pragma once

#include <cmath>
#include <vector>

namespace iron {

// Generates an RGBA8 normal map representing wooden plank seams along one
// axis. Plank-seam grooves run vertically (along V); the perturbed normal
// tilts away from each seam, creating the visual impression of a recessed
// groove between planks. `planks` is the number of plank columns across
// the texture. Output is `size * size * 4` bytes RGBA.
inline std::vector<unsigned char> generateWoodNormalMap(int size, int planks) {
    std::vector<unsigned char> out(static_cast<std::size_t>(size) * size * 4);
    const float invSize = 1.0f / static_cast<float>(size);
    const float twoPiPlanks = 2.0f * 3.14159265f * static_cast<float>(planks);
    const float strength = 0.3f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = (x + 0.5f) * invSize;
            // dN/dU: derivative of sin(2*pi*planks*u) wrt u gives the
            // slope, which becomes the X component of the tangent-space
            // normal. Sharp groove between planks: sin crosses zero at
            // each seam; cos at the seams is non-zero so the normal tilts.
            const float slope = std::cos(twoPiPlanks * u);
            const float nx = slope * strength;
            const float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx));
            const float ny = 0.0f;
            const int idx = (y * size + x) * 4;
            out[idx + 0] = static_cast<unsigned char>(
                (nx * 0.5f + 0.5f) * 255.0f);
            out[idx + 1] = static_cast<unsigned char>(
                (ny * 0.5f + 0.5f) * 255.0f);
            out[idx + 2] = static_cast<unsigned char>(
                (nz * 0.5f + 0.5f) * 255.0f);
            out[idx + 3] = 255;
        }
    }
    return out;
}

// Generates an RGBA8 specular mask: uniform high spec everywhere.
// Greyscale (R=G=B); the lit shader samples the R channel. Output is
// `size * size * 4` bytes RGBA.
inline std::vector<unsigned char> generateMetalSpecularMap(int size) {
    std::vector<unsigned char> out(static_cast<std::size_t>(size) * size * 4);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        out[i + 0] = 200;
        out[i + 1] = 200;
        out[i + 2] = 200;
        out[i + 3] = 255;
    }
    return out;
}

} // namespace iron
