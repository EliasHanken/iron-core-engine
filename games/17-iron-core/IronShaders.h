#pragma once

#include "common/ParticleLib.h"

#include "render/FluidVolume.h"
#include "render/GpuParticles.h"
#include "render/VolumeBounds.h"

#include <cstdint>
#include <string>

namespace ironcore {

using vfx::kParticleLib;
using vfx::kSoftDotFragment;

// Mirrors the GLSL Params block (std140: vec4s only).
struct IronParams {
    float phaseA[4];  // veins, lightning, ignition, flash
    float phaseB[4];  // rebirth, shockRadius, seconds, solidRadius
};

inline const iron::VolumeBounds kVolumeBounds{{-19.0f, -19.0f, -15.0f}, {19.0f, 19.0f, 15.0f}};

inline constexpr const char* kIronCommon = R"GLSL(
layout(set = 0, binding = 2) uniform Params {
    vec4 phaseA;
    vec4 phaseB;
} params;

#define VEINS        params.phaseA.x
#define LIGHTNING    params.phaseA.y
#define IGNITION     params.phaseA.z
#define FLASH        params.phaseA.w
#define REBIRTH      params.phaseB.x
#define SHOCK_RADIUS params.phaseB.y
#define SECONDS      params.phaseB.z
#define SOLID_RADIUS params.phaseB.w

const float CORE_RADIUS = 3.0;

vec3 randomDirection(uint seed) {
    float z = 2.0 * rnd(seed) - 1.0;
    float angle = 2.0 * PI * rnd(seed + 1u);
    float ring = sqrt(max(1.0 - z * z, 0.0));
    return vec3(ring * cos(angle), z, ring * sin(angle));
}
)GLSL";

// Perlin noise on a good integer hash. Plain value noise shows its grid at low
// frequency, which reads as blocks in the sky and on the molten surfaces.
inline constexpr const char* kGradientNoise = R"GLSL(
uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

vec3 gradientAt(vec3 cell) {
    uvec3 h = pcg3d(uvec3(ivec3(cell) + 100000));
    return vec3(h) / 4294967295.0 * 2.0 - 1.0;
}

// Roughly 0..1, centred on 0.5.
float perlin(vec3 p) {
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float n000 = dot(gradientAt(i + vec3(0, 0, 0)), f - vec3(0, 0, 0));
    float n100 = dot(gradientAt(i + vec3(1, 0, 0)), f - vec3(1, 0, 0));
    float n010 = dot(gradientAt(i + vec3(0, 1, 0)), f - vec3(0, 1, 0));
    float n110 = dot(gradientAt(i + vec3(1, 1, 0)), f - vec3(1, 1, 0));
    float n001 = dot(gradientAt(i + vec3(0, 0, 1)), f - vec3(0, 0, 1));
    float n101 = dot(gradientAt(i + vec3(1, 0, 1)), f - vec3(1, 0, 1));
    float n011 = dot(gradientAt(i + vec3(0, 1, 1)), f - vec3(0, 1, 1));
    float n111 = dot(gradientAt(i + vec3(1, 1, 1)), f - vec3(1, 1, 1));
    float n = mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
                  mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
    return clamp(0.5 + n * 0.85, 0.0, 1.0);
}

// Each octave is rotated, so no axis ever lines up.
float fbm(vec3 p, int octaves) {
    const mat3 turn = mat3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64);
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += perlin(p) * amplitude;
        total += amplitude;
        p = turn * p * 2.03 + vec3(1.7, 9.2, 3.1);
        amplitude *= 0.5;
    }
    return sum / total;
}
)GLSL";

// --- Sky ----------------------------------------------------------------------

inline constexpr const char* kSkyBody = R"GLSL(
float hash31(vec3 p) {
    uvec3 h = pcg3d(uvec3(ivec3(p) + 100000));
    return float(h.x) / 4294967295.0;
}

vec3 nebula(vec3 d) {
    vec3 warp = vec3(fbm(d * 1.4 + vec3(2.0, 7.0, 1.0), 5), fbm(d * 1.4 + vec3(8.0, 3.0, 5.0), 5),
                     fbm(d * 1.4 + vec3(4.0, 1.0, 9.0), 5));
    vec3 p = d * 1.8 + (warp - 0.5) * 3.2;

    float gas = fbm(p, 8);
    float filaments = pow(1.0 - abs(2.0 * fbm(p * 2.4 + warp * 2.0, 7) - 1.0), 7.0);
    float dust = smoothstep(0.50, 0.62, fbm(p * 1.5 + vec3(11.0, 2.0, 6.0), 7));

    // The nebula gathers in a broad band across the sky, leaving deep black.
    vec3 axis = normalize(vec3(0.25, 0.9, 0.35));
    float band = exp(-pow(dot(d, axis) * 2.4 + (warp.x - 0.5) * 4.0, 2.0));

    float density = smoothstep(0.44, 0.66, gas) * band;
    density *= density;

    vec3 violet = vec3(0.10, 0.02, 0.26);
    vec3 magenta = vec3(0.42, 0.03, 0.26);
    vec3 rust = vec3(0.80, 0.20, 0.04);
    vec3 teal = vec3(0.01, 0.16, 0.24);

    vec3 color = mix(violet, magenta, smoothstep(0.42, 0.62, warp.y));
    color = mix(color, rust, smoothstep(0.50, 0.62, warp.z) * smoothstep(0.50, 0.70, gas));
    color = mix(color, teal, smoothstep(0.54, 0.66, warp.x) * 0.7);

    vec3 result = color * density * 1.1;
    result += mix(magenta, rust, smoothstep(0.45, 0.6, warp.z)) * filaments * density * 1.6;
    result += vec3(0.004, 0.003, 0.010) * (0.3 + band);
    return result * (1.0 - dust * 0.92);
}

vec3 distantStars(vec3 d) {
    vec3 cell = floor(d * 170.0);
    float roll = hash31(cell);
    if (roll < 0.97) return vec3(0.0);
    vec3 offset = vec3(hash31(cell + 3.0), hash31(cell + 5.0), hash31(cell + 7.0)) - 0.5;
    vec3 center = (cell + 0.5 + offset * 0.6) / 170.0;
    float glow = smoothstep(0.0022, 0.0, length(d - normalize(center)));
    vec3 tint = mix(vec3(1.0, 0.8, 0.6), vec3(0.65, 0.8, 1.0), hash31(cell + 11.0));
    return tint * glow * (0.2 + 0.9 * hash31(cell + 13.0));
}

// A gas giant lit from behind, so that only a thin bright crescent shows.
vec3 gasGiant(vec3 d, vec3 background) {
    const vec3 center = normalize(vec3(-0.10, -0.30, -0.95));
    const float radius = 0.30;
    const vec3 sun = normalize(vec3(0.45, 0.55, -0.70));

    float angle = acos(clamp(dot(d, center), -1.0, 1.0));
    if (angle > radius * 1.25) return background;

    float h = angle / radius;
    vec3 side = normalize(d - center * dot(d, center));
    vec3 sunSide = normalize(sun - center * dot(sun, center));
    float towardSun = dot(side, sunSide);

    float glow = exp(-max(h - 1.0, 0.0) * 22.0) * smoothstep(-0.15, 0.95, towardSun);
    vec3 atmosphere = vec3(1.0, 0.42, 0.18) * glow * 1.4;
    if (h > 1.0) return background + atmosphere;

    // The surface faces the viewer, so its normal points back along the ray.
    vec3 normal = normalize(-center * sqrt(max(1.0 - h * h, 0.0)) + side * h);
    float lit = pow(max(dot(normal, sun), 0.0), 1.5);
    float bands = fbm(vec3(normal.y * 11.0, normal.x * 1.1, normal.z * 1.1), 5);
    vec3 surface = mix(vec3(0.35, 0.10, 0.06), vec3(1.0, 0.55, 0.26), smoothstep(0.35, 0.65, bands));
    return surface * lit * 0.9 + atmosphere * smoothstep(0.90, 1.0, h);
}

vec3 skyColor(vec3 dir) {
    return gasGiant(dir, nebula(dir) + distantStars(dir));
}
)GLSL";

inline std::string skyGlsl() { return std::string(kGradientNoise) + kSkyBody; }

// --- Surfaces -----------------------------------------------------------------

// params: x = veins, y = flash, z = seconds
inline constexpr const char* kShellBody = R"GLSL(
vec3 surfaceEmission(vec3 worldPos, vec3 params) {
    float veins = params.x;
    vec3 p = normalize(worldPos) * 3.0;
    float n = fbm(p * 0.9, 4);

    // Ridges of the noise are the fractures, widening as the core wakes up.
    float ridge = 1.0 - abs(2.0 * n - 1.0);
    float width = mix(0.995, 0.93, veins);
    float vein = smoothstep(width, width + 0.035, ridge);

    float pulse = 0.8 + 0.2 * sin(params.z * 3.0 + n * 20.0);
    vec3 hot = mix(vec3(1.0, 0.12, 0.01), vec3(1.0, 0.45, 0.10), vein * veins);
    return hot * vein * (0.3 + 6.0 * veins) * pulse + vec3(1.0, 0.3, 0.06) * params.y * 3.0;
}
)GLSL";

inline std::string shellEmission() { return std::string(kGradientNoise) + kShellBody; }

// params: x = veins, y = ignition, z = seconds
inline constexpr const char* kMoltenBody = R"GLSL(
vec3 surfaceEmission(vec3 worldPos, vec3 params) {
    vec3 p = normalize(worldPos);
    vec3 flow = vec3(params.z * 0.05, -params.z * 0.035, params.z * 0.04);
    float n = fbm(p * 2.4 + flow, 5);
    float cells = 1.0 - abs(2.0 * fbm(p * 4.5 - flow * 2.0, 4) - 1.0);
    n = clamp(n * 0.7 + cells * cells * 0.45, 0.0, 1.0);

    vec3 deep = vec3(0.8, 0.04, 0.01);
    vec3 orange = vec3(1.0, 0.30, 0.04);
    vec3 white = vec3(1.0, 0.84, 0.58);
    vec3 color = mix(deep, orange, smoothstep(0.40, 0.58, n));
    color = mix(color, white, smoothstep(0.74, 0.92, n));

    // params.y runs 0 to 1 as the core ignites, then 1 to 2 as the star is reborn.
    float exposed = 0.8 + 2.4 * n * n;
    float star = 1.6 + 7.0 * n * n;
    float level = params.y < 1.0 ? mix(0.12 + 2.4 * params.x, exposed, params.y)
                                 : mix(exposed, star, params.y - 1.0);
    return color * level;
}
)GLSL";

inline std::string moltenEmission() { return std::string(kGradientNoise) + kMoltenBody; }

// --- Plasma skin --------------------------------------------------------------

inline constexpr const char* kSkinSimulate = R"GLSL(
void particleSimulate(inout Particle p, uint id) {
    p.position.w += frame.dt;
    if (p.position.w >= p.velocity.w) {
        uint seed = id * 6971u + frame.frameIndex * 15485863u;
        vec3 dir = randomDirection(seed);
        p.position = vec4(dir * CORE_RADIUS * 1.03, 0.0);
        p.velocity = vec4(0.0, 0.0, 0.0, mix(0.5, 1.6, rnd(seed + 2u)));
        p.a = vec4(dir, rnd(seed + 3u));
        return;
    }
    vec3 x = p.position.xyz;
    vec3 dir = normalize(x);
    vec3 flow = curlNoise(x * 0.9 + vec3(0.0, SECONDS * 0.2, 0.0));
    vec3 along = flow - dir * dot(flow, dir);
    p.velocity.xyz = mix(p.velocity.xyz, along * 3.5, 1.0 - exp(-frame.dt * 9.0));
    x += p.velocity.xyz * frame.dt;
    p.position.xyz = normalize(x) * CORE_RADIUS * (1.03 + 0.02 * sin(SECONDS * 2.0 + p.a.w * 6.0));
    p.a.xyz = normalize(x);
}
)GLSL";

inline constexpr const char* kSkinVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    vec3 x = p.position.xyz;
    float life = clamp(p.position.w / max(p.velocity.w, 1e-3), 0.0, 1.0);
    float fade = smoothstep(0.0, 0.15, life) * (1.0 - smoothstep(0.7, 1.0, life));

    vec3 toCamera = normalize(frame.cameraPos.xyz - x);
    float facing = dot(p.a.xyz, toCamera);
    float nearSide = smoothstep(-0.1, 0.25, facing);
    float rim = pow(1.0 - abs(facing), 3.0);

    float strokes = pow(1.0 - abs(2.0 * valueNoise(x * 2.4 + vec3(0.0, SECONDS * 0.5, 0.0)) - 1.0), 6.0);
    vec3 tint = mix(vec3(1.0, 0.22, 0.03), vec3(1.0, 0.80, 0.50), strokes);

    float reborn = smoothstep(0.55, 1.0, REBIRTH);
    float visible = max(IGNITION * (1.0 - smoothstep(0.0, 0.3, REBIRTH)), reborn);
    float brightness = (0.03 + 0.5 * strokes + 0.5 * rim) * nearSide * fade * visible;
    color = vec4(tint * brightness * smokeTransmittance(x) * 0.7, 1.0);
    data = vec4(0.0);
    clipPos = stretchedSprite(x, p.velocity.xyz * 0.09, corner, 0.008 + 0.005 * p.a.w, 0.28);
}
)GLSL";

inline constexpr const char* kSkinEmission = R"GLSL(
vec3 particleEmission(Particle p, uint id) {
    float reborn = smoothstep(0.55, 1.0, REBIRTH);
    float visible = max(IGNITION * (1.0 - smoothstep(0.0, 0.3, REBIRTH)), reborn);
    return vec3(1.0, 0.45, 0.14) * visible * 0.006;
}
)GLSL";

// --- Magnetic field lines -----------------------------------------------------

inline constexpr const char* kFieldSimulate = R"GLSL(
const uint FIELD_LINES = 64u;

vec3 fieldPoint(float s, float shell, float azimuth) {
    float latitude = s * 1.25;
    float c = cos(latitude);
    float r = shell * c * c;
    vec3 local = vec3(r * c * cos(azimuth), r * sin(latitude), r * c * sin(azimuth));
    // The dipole leans, so it doesn't line up with the frame.
    float tilt = 0.32;
    return vec3(local.x * cos(tilt) - local.y * sin(tilt),
                local.x * sin(tilt) + local.y * cos(tilt), local.z);
}

void particleSimulate(inout Particle p, uint id) {
    uint line = id % FIELD_LINES;
    float shell = mix(5.5, 12.5, rnd(line * 31u + 1u));
    float azimuth = 2.0 * PI * rnd(line * 17u + 3u) + SECONDS * 0.05;
    float s = fract(rnd(id * 3u + 7u) + frame.time * (0.05 + 0.06 * rnd(line * 11u))) * 2.0 - 1.0;

    vec3 here = fieldPoint(s, shell, azimuth);
    vec3 next = fieldPoint(s + 0.012, shell, azimuth);
    vec3 scatter = (vec3(rnd(id * 5u), rnd(id * 7u), rnd(id * 11u)) - 0.5) * 0.05;

    p.position = vec4(here + scatter, 0.0);
    p.velocity = vec4(next - here, 1.0);
    p.a = vec4(s, shell, 0.0, 0.0);
}
)GLSL";

inline constexpr const char* kFieldVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    float r = length(p.position.xyz);
    float inside = smoothstep(CORE_RADIUS * 1.05, CORE_RADIUS * 1.5, r);

    float awake = smoothstep(0.4, 1.0, LIGHTNING);
    float blown = 1.0 - smoothstep(0.0, 0.2, IGNITION);
    float reborn = smoothstep(0.75, 1.0, REBIRTH) * 0.6;
    float visible = max(awake * blown, reborn);

    vec3 tint = mix(vec3(0.20, 0.42, 1.0), vec3(0.62, 0.20, 1.0), smoothstep(6.0, 12.0, p.a.y));
    color = vec4(tint * inside * visible * 0.05 * smokeTransmittance(p.position.xyz), 1.0);
    data = vec4(0.0);
    clipPos = stretchedSprite(p.position.xyz, p.velocity.xyz, corner, 0.006, 0.09);
}
)GLSL";

// --- Lightning ----------------------------------------------------------------

inline constexpr const char* kArcSimulate = R"GLSL(
const uint BOLTS = 14u;

vec3 boltPoint(float t, uint bolt, float tick) {
    uint seed = bolt * 97u + uint(tick) * 389u;
    vec3 a = randomDirection(seed);
    vec3 b = normalize(a + (randomDirection(seed + 9u)) * 0.9);

    // Arcs skim over the surface between two points on the shell.
    vec3 base = normalize(mix(a, b, t));
    float lift = sin(PI * t) * (0.35 + 1.1 * rnd(seed + 21u));
    float bolts = float(bolt);
    vec3 jitter = vec3(valueNoise(vec3(t * 9.0, bolts * 3.0, tick)),
                       valueNoise(vec3(t * 9.0, bolts * 7.0, tick + 3.0)),
                       valueNoise(vec3(t * 9.0, bolts * 5.0, tick + 7.0))) - 0.5;
    jitter += 0.4 * (vec3(valueNoise(vec3(t * 37.0, bolts, tick)),
                          valueNoise(vec3(t * 37.0, bolts * 2.0, tick + 1.0)),
                          valueNoise(vec3(t * 37.0, bolts * 4.0, tick + 2.0))) - 0.5);
    return base * (CORE_RADIUS + 0.05 + lift) + jitter * sin(PI * t) * 0.7;
}

void particleSimulate(inout Particle p, uint id) {
    uint bolt = id % BOLTS;
    float tick = floor(frame.time * 11.0 + float(bolt) * 0.37);
    float t = rnd(id * 13u + 5u);
    vec3 here = boltPoint(t, bolt, tick);
    vec3 next = boltPoint(min(t + 0.01, 1.0), bolt, tick);
    float live = rnd(bolt * 61u + uint(tick) * 7u) < 0.7 ? 1.0 : 0.0;
    p.position = vec4(here, 0.0);
    p.velocity = vec4(next - here, 1.0);
    p.a = vec4(live, t, 0.0, 0.0);
}
)GLSL";

inline constexpr const char* kArcVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    vec3 tint = mix(vec3(0.8, 0.9, 1.0), vec3(0.45, 0.55, 1.0), sin(PI * p.a.y));
    color = vec4(tint * p.a.x * LIGHTNING * 0.20, 1.0);
    data = vec4(0.0);
    clipPos = stretchedSprite(p.position.xyz, p.velocity.xyz, corner, 0.010, 0.05);
}
)GLSL";

inline constexpr const char* kArcEmission = R"GLSL(
vec3 particleEmission(Particle p, uint id) {
    return vec3(0.5, 0.65, 1.0) * p.a.x * LIGHTNING * 0.06;
}
)GLSL";

// --- Shock shell --------------------------------------------------------------

inline constexpr const char* kShockSimulate = R"GLSL(
void particleSimulate(inout Particle p, uint id) {
    vec3 dir = randomDirection(id * 3u + 11u);

    // The front is a disc as much as a shell, which reads better on screen.
    vec3 flattened = normalize(vec3(dir.x, dir.y * 0.28, dir.z));
    uint band = id % 3u;
    vec3 aim = band == 0u ? dir : flattened;

    float ripple = 0.5 * (valueNoise(aim * 3.0 + vec3(SECONDS * 0.4)) - 0.5);
    float thickness = (rnd(id * 19u) - 0.5) * 0.6;
    float radius = SHOCK_RADIUS * (1.0 + ripple * 0.25) + thickness;

    p.position = vec4(aim * radius, 0.0);
    p.velocity = vec4(aim * 1.0, 1.0);
    p.a = vec4(rnd(id * 23u), radius, 0.0, 0.0);
}
)GLSL";

inline constexpr const char* kShockVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    float r = p.a.y;
    float fade = (1.0 - smoothstep(14.0, 29.0, SHOCK_RADIUS)) * step(0.01, IGNITION);
    vec3 tint = mix(vec3(1.0, 0.9, 0.7), vec3(1.0, 0.32, 0.06), smoothstep(4.0, 15.0, r));
    // Stretched over the shell, the same particles stack up far less on screen as
    // it grows, so a small shell must draw dimmer per particle to stay even.
    float coverage = min(pow(SHOCK_RADIUS / 9.0, 2.0), 1.0);
    color = vec4(tint * fade * coverage * (1.0 - REBIRTH) * 0.09 * smokeTransmittance(p.position.xyz), 1.0);
    data = vec4(0.0);
    clipPos = stretchedSprite(p.position.xyz, p.velocity.xyz * 0.6, corner, 0.005 + 0.006 * p.a.x, 0.6);
}
)GLSL";

inline constexpr const char* kShockEmission = R"GLSL(
vec3 particleEmission(Particle p, uint id) {
    float fade = (1.0 - smoothstep(14.0, 29.0, SHOCK_RADIUS)) * step(0.01, IGNITION);
    // A young front packs every particle into a tiny shell, so its light is spread
    // by area, or it would flood the volume.
    float spread = min(pow(SHOCK_RADIUS / 14.0, 2.0), 1.0);
    return vec3(1.0, 0.5, 0.18) * fade * spread * 0.012;
}
)GLSL";

// --- Embers -------------------------------------------------------------------

// Thrown out by the blast, then swept into the accretion disc during rebirth.
inline constexpr const char* kEmberSimulate = R"GLSL(
void particleSimulate(inout Particle p, uint id) {
    if (p.velocity.w == 0.0) {
        vec3 dir = randomDirection(id * 5u + 3u);
        float ringRadius = mix(4.4, 15.0, pow(rnd(id * 7u + 1u), 1.7));
        p.position = vec4(dir * CORE_RADIUS, 0.0);
        p.velocity = vec4(0.0, 0.0, 0.0, 1.0);
        p.a = vec4(dir, ringRadius);
        p.b = vec4(rnd(id * 9u + 2u), 2.0 * PI * rnd(id * 11u + 4u), rnd(id * 13u), 0.0);
    }
    if (IGNITION <= 0.0) return;

    vec3 x = p.position.xyz;
    float dt = frame.dt;
    vec3 flow = curlNoise(x * 0.18 + vec3(SECONDS * 0.08));

    float launch = mix(9.0, 30.0, p.b.x * p.b.x);
    vec3 outward = normalize(x + vec3(1e-4));
    vec3 force = outward * launch * exp(-p.position.w * 1.4) * 2.0 + flow * 4.0;
    p.position.w += dt;

    vec3 v = p.velocity.xyz * exp(-dt * 0.9) + force * dt;
    vec3 previous = x;
    x += v * dt;

    if (REBIRTH > 0.0) {
        // The target orbit sits in the tilted ring plane.
        float angle = p.b.y + SECONDS * (22.0 / (p.a.w * sqrt(p.a.w)));
        vec3 flat_ = vec3(cos(angle) * p.a.w, (p.b.z - 0.5) * 0.45, sin(angle) * p.a.w);
        float tilt = 0.32;
        vec3 ring = vec3(flat_.x, flat_.y * cos(tilt) - flat_.z * sin(tilt),
                         flat_.y * sin(tilt) + flat_.z * cos(tilt));
        float pull = 1.0 - exp(-dt * mix(0.02, 2.2, REBIRTH * REBIRTH * REBIRTH));
        x = mix(x, ring, pull);
        v = (x - previous) / max(dt, 1e-3);
    }

    p.position.xyz = x;
    p.velocity.xyz = v;
}
)GLSL";

inline constexpr const char* kEmberVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    vec3 x = p.position.xyz;
    float r = length(x);
    float speed = length(p.velocity.xyz);
    vec3 tint = mix(vec3(1.0, 0.13, 0.02), vec3(1.0, 0.62, 0.22), clamp(speed * 0.06, 0.0, 1.0));
    vec3 discTint = mix(vec3(1.0, 0.72, 0.36), vec3(0.85, 0.10, 0.03), smoothstep(4.0, 13.0, r));
    tint = mix(tint, discTint, REBIRTH);
    float discFalloff = mix(1.0, 1.6 * exp(-max(r - 4.0, 0.0) * 0.28), REBIRTH);
    // Spiral arms, so that the settling disc never reads as a flat surface.
    float swirl = atan(x.z, x.x) * 2.0 + log(max(r, 1.0)) * 5.0 - SECONDS * 0.5;
    float arms = mix(1.0, 0.12 + 1.6 * pow(0.5 + 0.5 * sin(swirl), 3.0), REBIRTH);
    float brightness = mix(0.05, 0.26, REBIRTH * REBIRTH) * (0.15 + 0.85 * p.b.z * p.b.z) * discFalloff * arms;
    color = vec4(tint * brightness * step(0.01, IGNITION) * smokeTransmittance(x), 1.0);
    data = vec4(0.0);
    clipPos = stretchedSprite(x, p.velocity.xyz * mix(0.012, 0.03, REBIRTH * REBIRTH), corner, 0.007 + 0.008 * p.b.x, 0.30);
}
)GLSL";

inline constexpr const char* kEmberEmission = R"GLSL(
vec3 particleEmission(Particle p, uint id) {
    return vec3(1.0, 0.36, 0.09) * step(0.01, IGNITION) * 0.002;
}
)GLSL";

// --- Debris -------------------------------------------------------------------

// Small lit grit around the hero rocks, drawn as shaded spheres.
inline constexpr const char* kDebrisSimulate = R"GLSL(
void particleSimulate(inout Particle p, uint id) {
    if (p.velocity.w == 0.0) {
        float angle = 2.0 * PI * rnd(id * 3u + 1u);
        float radius = mix(5.0, 42.0, pow(rnd(id * 5u + 2u), 0.7));
        float height = (rnd(id * 7u + 3u) - 0.5) * 16.0;
        p.position = vec4(cos(angle) * radius, height, sin(angle) * radius, 0.0);
        p.velocity = vec4(0.0, 0.0, 0.0, 1.0);
        p.a = vec4(rnd(id * 11u), rnd(id * 13u), rnd(id * 17u), rnd(id * 19u));
    }

    vec3 x = p.position.xyz;
    float dist = length(x);
    vec3 outward = x / max(dist, 1e-3);

    // Slow orbit, then the blast front kicks each grain as it passes.
    vec3 orbit = cross(vec3(0.0, 1.0, 0.0), x) * 0.012;
    vec3 v = p.velocity.xyz * exp(-frame.dt * 0.35);
    if (SHOCK_RADIUS > dist && p.b.x == 0.0) {
        v += outward * mix(6.0, 22.0, p.a.x);
        p.b.x = 1.0;
    }
    if (REBIRTH > 0.0) v -= outward * REBIRTH * 7.0 * frame.dt;

    p.position.xyz = x + (v + orbit) * frame.dt;
    p.velocity.xyz = v;
}
)GLSL";

inline constexpr const char* kDebrisVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    vec3 x = p.position.xyz;
    float size = 0.006 + 0.022 * p.a.y * p.a.y * p.a.y;
    float toCamera = length(frame.cameraPos.xyz - x);
    float nearFade = smoothstep(1.5, 6.0, toCamera);
    float dist = length(x);

    float core = 0.01 + 0.9 * VEINS * VEINS * (1.0 - IGNITION) + 22.0 * IGNITION + 200.0 * FLASH;
    float light = core / (1.0 + dist * dist * 0.14);
    color = vec4(vec3(1.0, 0.46, 0.2) * light, nearFade);
    data = vec4(-x / max(dist, 1e-3), 0.0);

    vec4 viewPos = frame.view * vec4(x, 1.0);
    viewPos.xy += corner * size;
    clipPos = frame.projection * viewPos;
}
)GLSL";

inline constexpr const char* kDebrisFragment = R"GLSL(
vec4 particleFragment(vec2 corner, vec4 color, vec4 data) {
    float r2 = dot(corner, corner);
    if (r2 > 1.0) discard;

    vec3 viewNormal = vec3(corner, sqrt(1.0 - r2));
    vec3 normal = transpose(mat3(frame.view)) * viewNormal;
    float lit = max(dot(normal, data.xyz), 0.0);
    vec3 ambient = vec3(0.05, 0.035, 0.07);
    return vec4(color.rgb * lit * 0.22 + ambient, 1.0) * color.a;
}
)GLSL";

// --- Near stars ---------------------------------------------------------------

// A cubemap is too soft for pinpoint stars, so the brightest are sprites.
inline constexpr const char* kStarSimulate = R"GLSL(
void particleSimulate(inout Particle p, uint id) {
    if (p.velocity.w != 0.0) return;
    p.position = vec4(randomDirection(id * 3u + 1u) * 260.0, 0.0);
    p.velocity = vec4(0.0, 0.0, 0.0, 1.0);
    p.a = vec4(rnd(id * 5u), rnd(id * 7u), rnd(id * 11u), 0.0);
}
)GLSL";

inline constexpr const char* kStarVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    float bright = pow(p.a.x, 9.0);
    vec3 tint = mix(vec3(1.0, 0.78, 0.60), vec3(0.62, 0.78, 1.0), p.a.y);
    float twinkle = 0.85 + 0.15 * sin(SECONDS * (2.0 + 3.0 * p.a.z) + p.a.z * 40.0);
    color = vec4(tint * (0.10 + 4.0 * bright) * twinkle, 1.0);
    data = vec4(0.0);

    vec4 viewPos = frame.view * vec4(p.position.xyz, 1.0);
    viewPos.xy += corner * (0.05 + 0.20 * bright);
    clipPos = frame.projection * viewPos;
}
)GLSL";

// --- Effect descriptions ------------------------------------------------------

inline iron::GpuParticlesDesc makeEffect(std::uint32_t count, iron::ParticleBlend blend,
                                         const char* simulate, const char* vertex,
                                         const char* fragment, const char* emission = nullptr) {
    iron::GpuParticlesDesc desc;
    desc.count        = count;
    desc.blend        = blend;
    desc.paramsSize   = sizeof(IronParams);
    desc.commonGlsl   = std::string(kParticleLib) + kIronCommon;
    desc.simulateGlsl = simulate;
    desc.vertexGlsl   = vertex;
    desc.fragmentGlsl = fragment;
    if (emission) desc.emissionGlsl = emission;
    return desc;
}

inline iron::GpuParticlesDesc makeStarEffect() {
    return makeEffect(14'000, iron::ParticleBlend::Additive, kStarSimulate, kStarVertex,
                      kSoftDotFragment);
}
inline iron::GpuParticlesDesc makeDebrisEffect() {
    return makeEffect(70'000, iron::ParticleBlend::PremultipliedAlpha, kDebrisSimulate,
                      kDebrisVertex, kDebrisFragment);
}
inline iron::GpuParticlesDesc makeSkinEffect() {
    return makeEffect(420'000, iron::ParticleBlend::Additive, kSkinSimulate, kSkinVertex,
                      kSoftDotFragment, kSkinEmission);
}
inline iron::GpuParticlesDesc makeFieldEffect() {
    return makeEffect(260'000, iron::ParticleBlend::Additive, kFieldSimulate, kFieldVertex,
                      kSoftDotFragment);
}
inline iron::GpuParticlesDesc makeArcEffect() {
    return makeEffect(90'000, iron::ParticleBlend::Additive, kArcSimulate, kArcVertex,
                      kSoftDotFragment, kArcEmission);
}
inline iron::GpuParticlesDesc makeShockEffect() {
    return makeEffect(260'000, iron::ParticleBlend::Additive, kShockSimulate, kShockVertex,
                      kSoftDotFragment, kShockEmission);
}
inline iron::GpuParticlesDesc makeEmberEffect() {
    return makeEffect(380'000, iron::ParticleBlend::Additive, kEmberSimulate, kEmberVertex,
                      kSoftDotFragment, kEmberEmission);
}

// --- Fluid --------------------------------------------------------------------

inline constexpr const char* kFluidNoise = R"GLSL(
const float PI = 3.14159265;

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

float fbm(vec3 p, int octaves) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < octaves; ++i) {
        sum += valueNoise(p) * amplitude;
        p = p * 2.02 + vec3(1.7, 9.2, 3.1);
        amplitude *= 0.5;
    }
    return sum;
}

vec3 curlNoise(vec3 p) {
    const float e = 0.08;
    vec3 a = vec3(31.42, 17.13, 95.11);
    vec3 b = vec3(7.31, 81.97, 49.18);
    float dyz = valueNoise(p + vec3(0, e, 0) + b) - valueNoise(p - vec3(0, e, 0) + b);
    float dzy = valueNoise(p + vec3(0, 0, e) + a) - valueNoise(p - vec3(0, 0, e) + a);
    float dzx = valueNoise(p + vec3(0, 0, e)) - valueNoise(p - vec3(0, 0, e));
    float dxz = valueNoise(p + vec3(e, 0, 0) + b) - valueNoise(p - vec3(e, 0, 0) + b);
    float dxy = valueNoise(p + vec3(e, 0, 0) + a) - valueNoise(p - vec3(e, 0, 0) + a);
    float dyx = valueNoise(p + vec3(0, e, 0)) - valueNoise(p - vec3(0, e, 0));
    return vec3(dyz - dzy, dzx - dxz, dxy - dyx) / (2.0 * e);
}
)GLSL";

inline constexpr const char* kIronFluidParams = R"GLSL(
layout(set = 0, binding = 2) uniform Params {
    vec4 phaseA;
    vec4 phaseB;
} params;

#define VEINS        params.phaseA.x
#define LIGHTNING    params.phaseA.y
#define IGNITION     params.phaseA.z
#define FLASH        params.phaseA.w
#define REBIRTH      params.phaseB.x
#define SHOCK_RADIUS params.phaseB.y
#define SECONDS      params.phaseB.z
#define SOLID_RADIUS params.phaseB.w

const float CORE_RADIUS = 3.0;
)GLSL";

inline constexpr const char* kFluidSource = R"GLSL(
float fadeAtEdges(vec3 world) {
    // An uneven ellipsoid well inside the box, so that gas never finds a flat wall.
    vec3 scaled = world / vec3(18.0, 18.0, 14.0);
    float wobble = (fbm(world * 0.22 + vec3(5.0), 3) - 0.5) * 0.8;
    return smoothstep(1.0, 0.6, length(scaled) + wobble);
}

void fluidSource(vec3 world, inout vec3 velocity, inout float density, inout float heat) {
    float r = length(world);
    vec3 dir = world / max(r, 1e-3);
    float edge = fadeAtEdges(world);

    // Thin gas already hangs in the void, so the blast has something to sweep up.
    if (fluid.time < 0.05) {
        float gas = fbm(world * 0.17 + vec3(3.0), 6);
        gas = smoothstep(0.44, 0.60, gas) * smoothstep(3.5, 7.0, r);
        density = max(density, gas * 0.20 * edge);
    }

    // The cracks vent gas as the core wakes.
    float nearShell = exp(-max(r - CORE_RADIUS, 0.0) * 2.0) * (1.0 - IGNITION);
    float vents = smoothstep(0.52, 0.72, valueNoise(dir * 3.0 + vec3(0.0, SECONDS * 0.2, 0.0)));
    density += nearShell * vents * VEINS * 2.2 * fluid.dt;
    heat += nearShell * vents * VEINS * 1.6 * fluid.dt;
    velocity += dir * nearShell * vents * VEINS * 3.0 * fluid.dt;

    // Lightning stirs the gas.
    if (LIGHTNING > 0.0) {
        vec3 stir = curlNoise(world * 0.7 + vec3(floor(fluid.time * 11.0)));
        velocity += stir * LIGHTNING * exp(-max(r - CORE_RADIUS, 0.0) * 0.5) * 16.0 * fluid.dt;
    }

    // The blast front: everything it reaches is shoved outward and set alight.
    if (IGNITION > 0.0 && SHOCK_RADIUS < 34.0) {
        float width = 0.9 + 0.5 * valueNoise(dir * 4.0);
        float front = exp(-pow((r - SHOCK_RADIUS) / width, 2.0));
        float strength = 1.0 - smoothstep(10.0, 30.0, SHOCK_RADIUS);
        velocity += dir * front * strength * 420.0 * fluid.dt;
        heat += front * strength * 8.0 * fluid.dt;
        density += front * strength * edge * 1.1 * fluid.dt;

        // Hot gas left in the wake of the first second of the blast.
        float inside = smoothstep(SHOCK_RADIUS, SHOCK_RADIUS - 1.5, r) * FLASH;
        heat += inside * 1.2 * fluid.dt;
        density += inside * 0.8 * fluid.dt;
    }

    // Everything is drawn back in, spiralling into the reborn star's disc.
    if (REBIRTH > 0.0) {
        vec3 axis = normalize(vec3(0.0, cos(0.32), sin(0.32)));
        velocity -= dir * REBIRTH * 7.0 * fluid.dt;
        velocity += cross(axis, dir) * REBIRTH * 14.0 * fluid.dt;
        velocity -= axis * dot(world, axis) * REBIRTH * 2.0 * fluid.dt;
    }

    heat *= exp(-fluid.dt * mix(1.6, 0.8, REBIRTH));
    density *= mix(0.86, 1.0, edge);
    heat *= mix(0.86, 1.0, edge);
}
)GLSL";

inline constexpr const char* kFluidObstacle = R"GLSL(
float fluidObstacle(vec3 world) {
    return length(world) - SOLID_RADIUS;
}
)GLSL";

inline iron::FluidVolumeDesc makeFluidDesc() {
    iron::FluidVolumeDesc desc;
    desc.bounds = kVolumeBounds;
    desc.size = {208, 208, 164};
    desc.pressureIterations = 24;
    desc.vorticity = 5.0f;
    desc.velocityDissipation = 0.35f;
    desc.maxSpeed = 70.0f;
    desc.paramsSize = sizeof(IronParams);
    desc.commonGlsl = std::string(kFluidNoise) + kIronFluidParams;
    desc.sourceGlsl = kFluidSource;
    desc.obstacleGlsl = kFluidObstacle;
    return desc;
}

}  // namespace ironcore
