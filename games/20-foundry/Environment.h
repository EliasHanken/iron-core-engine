#pragma once

// The country round the works: hills rising out of the haze beyond the
// stockyard, dark with fir, and winter grass pushing up through the yard's
// cracked concrete along its walls and round what stands in it.
//
// The ground is one ring of mesh from under the stockyard's slab out to the
// camera's far plane. The trees are Poly Haven's fir_tree_01 (CC0) as impostors:
// far too heavy to draw (millions of triangles each), each was rendered from
// eight sides into an atlas (tools/foundry/bake_impostors.py), drawn as a card
// that turns to face the camera and shows the side nearest the one it's seen
// from, as AAA games draw distant trees. The grass is Poly Haven's
// grass_medium_01 clumps, drawn as they are. Both sway on the shader clock.

#include "Smoke.h"
#include "math/Transform.h"
#include "math/Vec.h"
#include "render/Renderer.h"
#include "render/StandardLitShader.h"
#include "scene/Mesh.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace foundry {

namespace environment {

// The land's height: flat under the stockyard, rising to a ring of hills.
inline float heightAt(float x, float z) {
    const float r = std::sqrt(x * x + z * z);
    const float a = std::atan2(z, x);
    const auto smooth = [](float e0, float e1, float v) {
        const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    // (All inside the camera's 400 m: the hills stand just short of it.)
    const float rise = smooth(245.0f, 385.0f, r);
    const float ridges = 26.0f + 14.0f * std::sin(a * 3.0f + 0.7f) + 9.0f * std::sin(a * 7.0f + 2.1f) +
                         5.0f * std::sin(a * 13.0f + 0.3f);
    const float rolls = 4.0f * std::sin(x * 0.021f + 1.3f) * std::sin(z * 0.017f + 0.4f) + 2.0f * std::sin(x * 0.07f) *
                                                                                              std::sin(z * 0.061f);
    return -0.15f + rise * ridges + smooth(215.0f, 320.0f, r) * rolls;
}

inline iron::MeshData terrain() {
    iron::MeshData m;
    constexpr int kRings = 80, kSegments = 256;
    const auto radius = [](int i) { return 140.0f * std::pow(395.0f / 140.0f, static_cast<float>(i) / kRings); };
    for (int i = 0; i <= kRings; ++i) {
        for (int j = 0; j <= kSegments; ++j) {
            const float a = 6.2831853f * static_cast<float>(j) / kSegments;
            const float r = radius(i);
            const float x = std::cos(a) * r, z = std::sin(a) * r;
            const float e = 0.5f;
            const Vec3 n = iron::normalize(Vec3{heightAt(x - e, z) - heightAt(x + e, z), 2.0f * e,
                                                heightAt(x, z - e) - heightAt(x, z + e)});
            m.vertices.push_back({{x, heightAt(x, z), z}, n, {x * 0.05f, z * 0.05f}, {1.0f, 0.0f, 0.0f}});
        }
    }
    for (int i = 0; i < kRings; ++i) {
        for (int j = 0; j < kSegments; ++j) {
            const auto v = [&](int a, int b) { return static_cast<std::uint32_t>(a * (kSegments + 1) + b); };
            m.indices.insert(m.indices.end(), {v(i, j), v(i + 1, j + 1), v(i + 1, j), v(i, j), v(i, j + 1), v(i + 1, j + 1)});  // Wound to face up.
        }
    }
    return m;
}

// One card a unit wide and tall, its foot on the origin, facing +z; the
// impostor shader turns it to the camera and sizes it.
inline iron::MeshData card() {
    iron::MeshData m;
    const Vec3 n{0.0f, 0.0f, 1.0f}, t{1.0f, 0.0f, 0.0f};
    m.vertices = {{{-0.5f, 0.0f, 0.0f}, n, {0.0f, 1.0f}, t},
                  {{0.5f, 0.0f, 0.0f}, n, {1.0f, 1.0f}, t},
                  {{0.5f, 1.0f, 0.0f}, n, {1.0f, 0.0f}, t},
                  {{-0.5f, 1.0f, 0.0f}, n, {0.0f, 0.0f}, t}};
    m.indices = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    return m;
}

// The instanced lit shader as an impostor: the card stands at its instance's
// origin, turned about the vertical to face the camera, `width` x `height` metres
// times the instance's scale, and samples the one of the atlas's `views` nearest
// the side it's seen from (the instance's own turn included).
inline std::string impostorVertex(int views, float width, float height) {
    std::string src = iron::standardLitInstancedVertSource();
    const std::string from = "    vec4 world = model * vec4(aPos, 1.0);";
    const std::string to =
        "    vec3 root = model[3].xyz;\n"
        "    float tall = length(model[1].xyz);\n"
        "    vec2 toEye = u.cameraPos.xz - root.xz;\n"
        "    toEye = length(toEye) > 1e-3 ? normalize(toEye) : vec2(0.0, 1.0);\n"
        "    vec3 across = vec3(toEye.y, 0.0, -toEye.x);\n"
        "    vec4 world = vec4(root + across * aPos.x * " + std::to_string(width) + " * tall + vec3(0.0, aPos.y * " +
        std::to_string(height) + " * tall, 0.0), 1.0);\n"
        "    float t = u.cameraPos.w;\n"
        "    world.xz += vec2(0.8, 0.45) * sin(t * 0.9 + root.x * 0.13 + root.z * 0.11) * 0.012 * tall * aPos.y * aPos.y;\n"
        "    float turn = atan(model[0].z, model[0].x);\n"
        "    float seen = atan(toEye.x, toEye.y) + turn;\n"
        "    float view = mod(floor(seen / 6.2831853 * " + std::to_string(views) + ".0 + 0.5), " + std::to_string(views) +
        ".0);\n";
    src.replace(src.find(from), from.size(), to);
    const std::string uvFrom = "    vUV = aUV;";
    src.replace(src.find(uvFrom), uvFrom.size(),
                "    vUV = vec2((view + aUV.x) / " + std::to_string(views) + ".0, aUV.y);");
    const std::string nFrom = "    vNormal = mat3(model) * aNormal;";
    src.replace(src.find(nFrom), nFrom.size(), "    vNormal = normalize(vec3(toEye.x, 1.2, toEye.y));");
    const std::string tFrom = "    vTangent = mat3(model) * aTangent;";
    src.replace(src.find(tFrom), tFrom.size(), "    vTangent = across;");
    return src;
}

// The instanced lit shader, swaying: the top of a card moves most, by `sway`
// metres per metre of height, in a gusting breeze off the hills.
inline std::string swayingVertex(float sway) {
    std::string src = iron::standardLitInstancedVertSource();
    const std::string from = "    vec4 world = model * vec4(aPos, 1.0);";
    const std::string to =
        "    vec4 world = model * vec4(aPos, 1.0);\n"
        "    float tall = length(model[1].xyz);\n"
        "    float t = u.cameraPos.w;\n"
        "    float gust = 0.6 + 0.4 * sin(t * 0.37 + world.x * 0.011 + world.z * 0.007);\n"
        "    float wave = sin(t * 1.7 + world.x * 0.21 + world.z * 0.17) + 0.4 * sin(t * 3.3 + world.z * 0.43);\n"
        "    world.xz += vec2(0.8, 0.45) * wave * gust * " + std::to_string(sway) + " * tall * aPos.y * aPos.y;\n";
    src.replace(src.find(from), from.size(), to);
    return src;
}

// Firs on the hills, thicker where the ground rises, none on the works; split
// among `variants` (each a different tree), sized round the size they were baked.
inline std::vector<std::vector<iron::Mat4>> forest(int variants, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::vector<std::vector<iron::Mat4>> out(static_cast<std::size_t>(variants));
    for (int placed = 0; placed < 7000;) {
        const float r = 225.0f + 158.0f * std::sqrt(u(rng));
        const float a = 6.2831853f * u(rng);
        const float x = std::cos(a) * r, z = std::sin(a) * r;
        if (std::abs(x) < 185.0f && z > -125.0f && z < 190.0f) continue;  // The stockyard.
        const float h = heightAt(x, z);
        if (u(rng) > 0.25f + std::clamp(h / 20.0f, 0.0f, 0.75f)) continue;  // Thinner on the flat.
        // In stands, with clearings between.
        const float stands = std::sin(x * 0.045f + 1.1f) * std::sin(z * 0.052f + 0.3f) + 0.5f * std::sin(a * 11.0f);
        if (stands < -0.35f && u(rng) < 0.85f) continue;
        const float size = 0.75f + 0.55f * u(rng);
        out[static_cast<std::size_t>(placed % variants)].push_back(
            iron::translation({x, h - 0.3f, z}) * iron::rotationY(6.2831853f * u(rng)) * iron::scaling({size, size, size}));
        ++placed;
    }
    return out;
}

// Grass in the yard: clumps along the foot of everything standing in it, and a
// scatter through the cracks; none on the hall's floor. Split among the clumps
// whose centres (in their own model's space, to cancel) are `centres`.
inline std::vector<std::vector<iron::Mat4>> yardGrass(std::span<const SmokeCollider> boxes, std::span<const Vec3> centres,
                                                      std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::vector<std::vector<iron::Mat4>> out(centres.size());
    const auto inside = [&](float x, float z) {
        for (const SmokeCollider& b : boxes) {
            if (b.centre.y - b.half.y > 0.3f || b.centre.y + b.half.y < 0.1f) continue;  // Overhead, or the floor.
            const float c = std::cos(b.yaw), s = std::sin(b.yaw);
            const float dx = x - b.centre.x, dz = z - b.centre.z;
            const float lx = dx * c - dz * s, lz = dx * s + dz * c;
            if (std::abs(lx) < b.half.x + 0.05f && std::abs(lz) < b.half.z + 0.05f) return true;
        }
        return false;
    };
    std::size_t next = 0;
    const auto clump = [&](float x, float z, float size) {
        const std::size_t k = next++ % centres.size();
        out[k].push_back(iron::translation({x, 0.0f, z}) * iron::rotationY(6.2831853f * u(rng)) *
                         iron::scaling({size, size, size}) * iron::translation({-centres[k].x, 0.0f, -centres[k].z}));
    };
    const auto allowed = [&](float x, float z) {
        if (std::abs(x) > 33.5f || z > 35.5f || z < -69.0f) return false;
        if (z < -20.5f && std::abs(x) < 16.0f) return false;  // The hall.
        return !inside(x, z);
    };
    // Round the feet of walls, containers, crates: where the wind leaves dirt.
    for (const SmokeCollider& b : boxes) {
        if (b.body.value != iron::kInvalidBody.value || b.centre.y - b.half.y > 0.3f || b.half.y < 0.3f) continue;
        const int count = static_cast<int>(4.0f * (b.half.x + b.half.z) * 1.1f);
        const float c = std::cos(b.yaw), s = std::sin(b.yaw);
        for (int i = 0; i < count; ++i) {
            const float along = u(rng) * 2.0f - 1.0f;
            const int face = static_cast<int>(u(rng) * 3.99f);
            const float off = 0.12f + 0.3f * u(rng);
            const float lx = face < 2 ? (face == 0 ? -1.0f : 1.0f) * (b.half.x + off) : along * b.half.x;
            const float lz = face < 2 ? along * b.half.z : (face == 2 ? -1.0f : 1.0f) * (b.half.z + off);
            const float x = b.centre.x + lx * c + lz * s, z = b.centre.z - lx * s + lz * c;
            if (allowed(x, z)) clump(x, z, 1.4f + 1.4f * u(rng));
        }
    }
    // And through the cracks.
    for (int i = 0; i < 900; ++i) {
        const float x = -33.0f + 66.0f * u(rng), z = -19.0f + 54.0f * u(rng);
        if (allowed(x, z)) clump(x, z, 1.0f + 0.9f * u(rng));
    }
    return out;
}

}  // namespace environment

}  // namespace foundry
