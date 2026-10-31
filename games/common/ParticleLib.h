#pragma once

namespace vfx {

// GLSL shared by the demo's effects. Goes in GpuParticlesDesc::commonGlsl.
inline constexpr const char* kParticleLib = R"GLSL(
const float PI = 3.14159265;

uint pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

float rnd(uint seed) { return float(pcg(seed)) / 4294967295.0; }

float hash31(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float valueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(mix(hash31(i + vec3(0, 0, 0)), hash31(i + vec3(1, 0, 0)), f.x),
            mix(hash31(i + vec3(0, 1, 0)), hash31(i + vec3(1, 1, 0)), f.x), f.y),
        mix(mix(hash31(i + vec3(0, 0, 1)), hash31(i + vec3(1, 0, 1)), f.x),
            mix(hash31(i + vec3(0, 1, 1)), hash31(i + vec3(1, 1, 1)), f.x), f.y),
        f.z);
}

vec3 potential(vec3 p) {
    return vec3(valueNoise(p),
                valueNoise(p + vec3(31.42, 17.13, 95.11)),
                valueNoise(p + vec3(7.31, 81.97, 49.18)));
}

vec3 curlNoise(vec3 p) {
    const float e = 0.04;
    vec3 dx = (potential(p + vec3(e, 0, 0)) - potential(p - vec3(e, 0, 0))) / (2.0 * e);
    vec3 dy = (potential(p + vec3(0, e, 0)) - potential(p - vec3(0, e, 0))) / (2.0 * e);
    vec3 dz = (potential(p + vec3(0, 0, e)) - potential(p - vec3(0, 0, e))) / (2.0 * e);
    return vec3(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x);
}

// Stretches a sprite along its screen-space motion, which is what turns dots
// into the wiry filaments.
vec4 stretchedSprite(vec3 world, vec3 motion, vec2 corner, float width, float maxLength) {
    vec4 viewPos = frame.view * vec4(world, 1.0);
    vec2 dir = (frame.view * vec4(motion, 0.0)).xy;
    float len = length(dir);
    dir = len > 1e-5 ? dir / len : vec2(0.0, 1.0);
    vec2 side = vec2(-dir.y, dir.x);
    viewPos.xy += side * corner.x * width + dir * corner.y * max(width, min(len, maxLength));
    return frame.projection * viewPos;
}
)GLSL";

// The default look: a soft round dot of light, for additive effects.
inline constexpr const char* kSoftDotFragment = R"GLSL(
vec4 particleFragment(vec2 corner, vec4 color, vec4 data) {
    float r = 1.0 - dot(corner, corner);
    if (r <= 0.0) discard;
    return vec4(color.rgb * r * r, 0.0);
}
)GLSL";

}  // namespace vfx
