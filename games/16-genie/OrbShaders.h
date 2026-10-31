#pragma once

#include "ParticleLib.h"

#include "render/GpuParticles.h"

#include <cstdint>
#include <string>

namespace genie {

// A sphere of light combed by a flow field. Most particles stay on the surface
// and trace the field as hatched strokes; a few break free and stream away
// into the dark.
inline constexpr const char* kOrbCommon = R"GLSL(
const float ORB_RADIUS = 2.6;

float orbRadius(vec3 dir) {
    float lumps = valueNoise(dir * 1.8 + vec3(0.0, frame.time * 0.18, 0.0)) - 0.5;
    return ORB_RADIUS * (1.0 + 0.14 * lumps);
}
)GLSL";

inline constexpr const char* kOrbSimulate = R"GLSL(
void spawn(inout Particle p, uint id) {
    uint seed = id * 6971u + frame.frameIndex * 15485863u;
    float z = 2.0 * rnd(seed) - 1.0;
    float angle = 2.0 * PI * rnd(seed + 1u);
    float ring = sqrt(1.0 - z * z);
    vec3 dir = vec3(ring * cos(angle), z, ring * sin(angle));

    bool stream = id % 9u == 0u;
    p.position = vec4(dir * orbRadius(dir), 0.0);
    p.velocity = vec4(0.0, 0.0, 0.0,
                      stream ? mix(1.0, 3.5, rnd(seed + 2u)) : mix(0.6, 1.8, rnd(seed + 2u)));
    p.a = vec4(dir, 0.0);
    p.b = vec4(stream ? 1.0 : 0.0, rnd(seed + 3u), 0.0, 0.0);
}

void particleSimulate(inout Particle p, uint id) {
    p.position.w += frame.dt;
    if (p.position.w >= p.velocity.w) {
        spawn(p, id);
        return;
    }

    vec3 x = p.position.xyz;
    vec3 dir = normalize(x);
    vec3 flow = curlNoise(x * 0.75 + vec3(0.0, frame.time * 0.22, 0.0));

    if (p.b.x > 0.5) {
        vec3 target = normalize(dir * 0.9 + flow * 1.1) * 3.0;
        p.velocity.xyz = mix(p.velocity.xyz, target, 1.0 - exp(-frame.dt * 4.0));
        p.position.xyz = x + p.velocity.xyz * frame.dt;
    } else {
        vec3 along = flow - dir * dot(flow, dir);
        p.velocity.xyz = mix(p.velocity.xyz, along * 3.2, 1.0 - exp(-frame.dt * 9.0));
        x += p.velocity.xyz * frame.dt;
        dir = normalize(x);
        p.position.xyz = dir * orbRadius(dir);
    }
    p.a.xyz = dir;
}
)GLSL";

inline constexpr const char* kOrbVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    vec3 x = p.position.xyz;
    vec3 dir = p.a.xyz;
    float life = clamp(p.position.w / max(p.velocity.w, 1e-3), 0.0, 1.0);
    bool stream = p.b.x > 0.5;
    data = vec4(0.0);

    if (stream) {
        float fade = smoothstep(0.0, 0.1, life) * (1.0 - life) * (1.0 - life);
        float hue = smoothstep(0.0, 0.5, life);
        vec3 tint = mix(vec3(0.30, 0.34, 1.0), vec3(0.42, 0.08, 1.0), hue);
        color = vec4(tint * fade * 0.20, 1.0);
        clipPos = stretchedSprite(x, p.velocity.xyz * 0.16, corner, 0.008 + 0.006 * p.b.y, 0.55);
        return;
    }

    float fade = smoothstep(0.0, 0.15, life) * (1.0 - smoothstep(0.7, 1.0, life));
    vec3 toCamera = normalize(frame.cameraPos.xyz - x);
    float facing = dot(dir, toCamera);
    float nearSide = mix(0.08, 1.0, smoothstep(-0.15, 0.2, facing));
    float rim = pow(1.0 - abs(facing), 3.0);

    // Bands of the surface light up in turn, so the hatching reads as strokes.
    float strokes = 1.0 - abs(2.0 * valueNoise(x * 3.4 + vec3(0.0, frame.time * 0.5, 0.0)) - 1.0);
    strokes = pow(strokes, 7.0);

    vec3 tint = mix(vec3(0.14, 0.26, 1.0), vec3(0.70, 0.90, 1.0), strokes);
    float brightness = (0.02 + 0.30 * strokes + 0.16 * rim) * nearSide * fade;
    color = vec4(tint * brightness * 0.42, 1.0);
    clipPos = stretchedSprite(x, p.velocity.xyz * 0.11, corner, 0.006 + 0.004 * p.b.y, 0.30);
}
)GLSL";

// --- Floor grid ---------------------------------------------------------------

inline constexpr std::uint32_t kGridLinesPerAxis = 61;

inline constexpr const char* kGridSimulate = R"GLSL(
void particleSimulate(inout Particle p, uint id) {}
)GLSL";

// Each sprite is one whole grid line, so this "particle system" is two quads per
// metre of floor and no geometry at all.
inline constexpr const char* kGridVertex = R"GLSL(
const float GRID_Y = -4.4;
const float GRID_HALF_EXTENT = 30.0;
const uint LINES_PER_AXIS = 61u;

void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    bool alongX = id >= LINES_PER_AXIS;
    float offset = float(id % LINES_PER_AXIS) - GRID_HALF_EXTENT;
    float along = corner.y * GRID_HALF_EXTENT;
    float across = offset + corner.x * 0.012;

    vec3 world = alongX ? vec3(along, GRID_Y, across) : vec3(across, GRID_Y, along);
    color = vec4(0.13, 0.13, 0.19, 1.0);
    data = vec4(world.xz, corner.x, 0.0);
    clipPos = frame.projection * frame.view * vec4(world, 1.0);
}
)GLSL";

inline constexpr const char* kGridFragment = R"GLSL(
vec4 particleFragment(vec2 corner, vec4 color, vec4 data) {
    float distance = length(data.xy);
    return vec4(color.rgb * exp(-distance * 0.11) * (1.0 - data.z * data.z), 0.0);
}
)GLSL";

inline iron::GpuParticlesDesc makeOrbEffect() {
    iron::GpuParticlesDesc desc;
    desc.count        = 800'000;
    desc.commonGlsl   = std::string(kParticleLib) + kOrbCommon;
    desc.simulateGlsl = kOrbSimulate;
    desc.vertexGlsl   = kOrbVertex;
    desc.fragmentGlsl = kSoftDotFragment;
    return desc;
}

inline iron::GpuParticlesDesc makeGridEffect() {
    iron::GpuParticlesDesc desc;
    desc.count        = 2 * kGridLinesPerAxis;
    desc.commonGlsl   = kParticleLib;
    desc.simulateGlsl = kGridSimulate;
    desc.vertexGlsl   = kGridVertex;
    desc.fragmentGlsl = kGridFragment;
    return desc;
}

}  // namespace genie
