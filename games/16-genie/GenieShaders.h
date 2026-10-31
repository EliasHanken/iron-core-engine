#pragma once

#include "common/ParticleLib.h"

#include "render/GpuParticles.h"

#include <cstdint>
#include <string>

namespace genie {

using vfx::kParticleLib;
using vfx::kSoftDotFragment;

// Mirrors the GLSL Params block in kCommon (std140: vec4s only).
struct GenieParams {
    float spout[4];
    float streamControl1[4];
    float streamControl2[4];
    float streamEnd[4];
    float phaseA[4];  // stream, smoke, reveal, dissolve
    float phaseB[4];  // eyes, seconds, flicker, unused
};

// The genie has no mesh: it is a signed distance field, so its body is just the
// surface a million particles are pulled onto.
inline constexpr const char* kCommon = R"GLSL(
layout(set = 0, binding = 2) uniform Params {
    vec4 spout;
    vec4 streamControl1;
    vec4 streamControl2;
    vec4 streamEnd;
    vec4 phaseA;
    vec4 phaseB;
} params;

#define STREAM   params.phaseA.x
#define SMOKE    params.phaseA.y
#define REVEAL   params.phaseA.z
#define DISSOLVE params.phaseA.w
#define EYES     params.phaseB.x
#define SECONDS  params.phaseB.y
#define FLICKER  params.phaseB.z

const vec3 GENIE_ORIGIN = vec3(0.5, 0.0, 0.0);

// Must stay in lockstep with genie::groundHeight in GenieScene.cpp.
float groundHeight(vec2 xz) {
    return 0.70 * sin(0.21 * xz.x + 0.6 * sin(0.13 * xz.y)) +
           0.35 * sin(0.37 * xz.y + 0.19 * xz.x + 1.3) +
           0.12 * sin(0.83 * xz.x - 0.47 * xz.y);
}

vec3 streamPoint(float t) {
    float k = 1.0 - t;
    return k * k * k * params.spout.xyz + 3.0 * k * k * t * params.streamControl1.xyz +
           3.0 * k * t * t * params.streamControl2.xyz + t * t * t * params.streamEnd.xyz;
}

vec3 streamTangent(float t) {
    float k = 1.0 - t;
    return normalize(3.0 * k * k * (params.streamControl1.xyz - params.spout.xyz) +
                     6.0 * k * t * (params.streamControl2.xyz - params.streamControl1.xyz) +
                     3.0 * t * t * (params.streamEnd.xyz - params.streamControl2.xyz));
}

// The upper body sways, pivoting at the waist.
float genieSway(float y) {
    return 0.20 * sin(SECONDS * 0.7) * smoothstep(2.0, 7.0, y);
}

float capsule(vec3 p, vec3 a, vec3 b, float r0, float r1) {
    vec3 pa = p - a;
    vec3 ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - mix(r0, r1, h);
}

float smoothMin(float a, float b, float k) {
    float h = max(k - abs(a - b), 0.0) / k;
    return min(a, b) - h * h * k * 0.25;
}

float genieSdf(vec3 world) {
    vec3 p = world - GENIE_ORIGIN;
    p.x -= genieSway(p.y);

    float breath = 0.03 * sin(SECONDS * 1.6);

    // Torso, flattened front to back.
    vec3 chest = p * vec3(1.0, 1.0, 1.4);
    float d = capsule(chest, vec3(0, 3.0, 0), vec3(0, 5.0, 0), 0.50, 1.00 + breath);

    // Shoulders, neck and head.
    d = smoothMin(d, capsule(chest, vec3(-1.10, 5.25, 0), vec3(1.10, 5.25, 0), 0.46, 0.46), 0.5);
    d = smoothMin(d, capsule(p, vec3(0, 5.5, 0), vec3(0, 6.2, 0.05), 0.36, 0.30), 0.25);
    vec3 skull = (p - vec3(0, 6.55, 0.08)) * vec3(1.0, 0.85, 1.0);
    d = smoothMin(d, length(skull) - 0.52, 0.15);

    // Arms folded across the chest.
    float arms = capsule(p, vec3(-1.42, 5.20, 0), vec3(-1.72, 3.55, 0.45), 0.42, 0.30);
    arms = smoothMin(arms, capsule(p, vec3(-1.72, 3.55, 0.45), vec3(0.80, 4.45, 1.20), 0.30, 0.20), 0.15);
    arms = smoothMin(arms, capsule(p, vec3(1.42, 5.20, 0), vec3(1.72, 3.40, 0.45), 0.42, 0.30), 0.15);
    arms = smoothMin(arms, capsule(p, vec3(1.72, 3.40, 0.45), vec3(-0.80, 3.75, 1.45), 0.30, 0.20), 0.15);
    d = smoothMin(d, arms, 0.15);

    // Tail, tapering down towards the stream.
    d = smoothMin(d, capsule(p, vec3(0, 3.1, 0), vec3(-0.05, 2.0, 0), 0.52, 0.32), 0.4);
    d = smoothMin(d, capsule(p, vec3(-0.05, 2.0, 0), vec3(-0.35, 1.1, 0.1), 0.34, 0.14), 0.2);
    return d;
}

vec3 genieNormal(vec3 p) {
    const float e = 0.015;
    const vec2 k = vec2(1.0, -1.0);
    return normalize(k.xyy * genieSdf(p + k.xyy * e) + k.yyx * genieSdf(p + k.yyx * e) +
                     k.yxy * genieSdf(p + k.yxy * e) + k.xxx * genieSdf(p + k.xxx * e));
}

// World position of a point given in genie-local coordinates.
vec3 genieLocal(vec3 p) {
    return GENIE_ORIGIN + vec3(p.x + genieSway(p.y), p.y, p.z);
}

)GLSL";

// --- Genie body ---------------------------------------------------------------

inline constexpr const char* kBodySimulate = R"GLSL(
void spawn(inout Particle p, uint id) {
    uint seed = id * 7919u + frame.frameIndex * 104729u;
    vec3 q = vec3(mix(-2.0, 3.0, rnd(seed)),
                  mix(1.0, 7.2, rnd(seed + 1u)),
                  mix(-1.4, 2.0, rnd(seed + 2u)));
    for (int i = 0; i < 6; ++i) {
        q -= genieNormal(q) * genieSdf(q);
    }
    p.position = vec4(q, 0.0);
    p.velocity = vec4(0.0, 0.0, 0.0, mix(0.9, 2.4, rnd(seed + 3u)));
    p.a = vec4(genieNormal(q), 0.0);
    // A few particles are embers that escape the surface.
    p.b = vec4(rnd(seed + 4u), id % 48u == 0u ? 1.0 : 0.0, 0.0, 0.0);
}

void particleSimulate(inout Particle p, uint id) {
    p.position.w += frame.dt;
    if (p.position.w >= p.velocity.w) {
        spawn(p, id);
        return;
    }

    vec3 x = p.position.xyz;
    float d = genieSdf(x);
    vec3 n = genieNormal(x);
    vec3 flow = curlNoise(x * 0.85 + vec3(0.0, -SECONDS * 0.4, 0.0));

    vec3 along = flow * 1.6 + vec3(0.0, 1.1, 0.0);
    along -= n * dot(along, n);
    vec3 target = along * 1.9 - n * clamp(d, -0.6, 0.6) * 7.0;

    float ember = p.b.y * smoothstep(0.35, 0.6, p.position.w / p.velocity.w);
    target = mix(target, flow * 1.2 + n * 0.4 + vec3(0.0, 1.0, 0.0), ember);
    target = mix(target, flow * 3.5 + n * 1.5 + vec3(0.0, 1.4, 0.0), DISSOLVE);

    vec3 v = mix(p.velocity.xyz, target, 1.0 - exp(-frame.dt * 6.0));
    p.position.xyz = x + v * frame.dt;
    p.velocity.xyz = v;

    // Bright veins crawl over the skin where the noise crosses its midpoint.
    float ridge = 1.0 - abs(2.0 * valueNoise(x * 2.6 + vec3(0.0, -SECONDS * 0.6, 0.0)) - 1.0);
    p.a = vec4(n, pow(ridge, 6.0));
}
)GLSL";

inline constexpr const char* kBodyVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    vec3 x = p.position.xyz;
    float life = clamp(p.position.w / max(p.velocity.w, 1e-3), 0.0, 1.0);
    float fade = smoothstep(0.0, 0.12, life) * (1.0 - smoothstep(0.75, 1.0, life));

    // The body forms from the bottom up behind a noisy edge.
    float edge = mix(0.0, 8.8, REVEAL) +
                 1.7 * (valueNoise(x * 0.9 + vec3(0.0, -SECONDS * 0.5, 0.0)) - 0.5);
    float formed = smoothstep(0.25, -0.25, x.y - edge);
    float front = exp(-abs(x.y - edge) * 4.0) * step(0.001, REVEAL) * (1.0 - REVEAL);

    // Hot at the silhouette, so the body reads as a glowing shell.
    vec3 toCamera = normalize(frame.cameraPos.xyz - x);
    float facing = dot(p.a.xyz, toCamera);
    float rim = pow(1.0 - abs(facing), 2.5);
    float nearSide = mix(0.12, 1.0, smoothstep(-0.25, 0.15, facing));

    float height = clamp((x.y - 1.0) / 6.2, 0.0, 1.0);
    vec3 low  = vec3(1.0, 0.03, 0.42);
    vec3 mid  = vec3(1.0, 0.08, 0.05);
    vec3 high = vec3(1.0, 0.20, 0.03);
    vec3 head = vec3(0.22, 0.10, 1.0);
    vec3 tint = mix(low, mid, smoothstep(0.05, 0.40, height));
    tint = mix(tint, high, smoothstep(0.40, 0.70, height));
    tint = mix(tint, head, smoothstep(0.78, 0.88, height));

    float veins = p.a.w;
    // The head stays dim enough for the eyes to stand out against it.
    float headDim = mix(1.0, 0.4, smoothstep(0.78, 0.88, height));
    float brightness = (0.015 + 0.22 * rim) * (0.10 + 8.0 * veins) * nearSide * headDim +
                       0.4 * front;

    float ember = p.b.y * smoothstep(0.35, 0.6, life);
    brightness = mix(brightness, 0.5, ember);

    float dissolve = 1.0 - smoothstep(0.55, 1.0, DISSOLVE);
    color = vec4(tint * brightness * fade * formed * dissolve * 0.55, 1.0);
    data = vec4(0.0);

    float width = 0.007 + 0.005 * p.b.x;
    clipPos = stretchedSprite(x, p.velocity.xyz * 0.13, corner, width, mix(0.40, 0.10, ember));
}
)GLSL";

// --- Plasma stream ------------------------------------------------------------

inline constexpr const char* kPlasmaSimulate = R"GLSL(
const uint FILAMENTS = 7u;

// One filament is a jittered path that jumps to a new shape many times a second,
// which is what makes it read as electricity rather than a tube.
vec3 filamentPoint(float t, float filament) {
    float tick = floor(frame.time * 13.0 + filament * 0.37);
    vec2 o = vec2(valueNoise(vec3(t * 7.0, filament * 9.1, tick)),
                  valueNoise(vec3(t * 7.0, filament * 5.3 + 40.0, tick))) - 0.5;
    o += 0.35 * (vec2(valueNoise(vec3(t * 31.0, filament * 3.7, tick)),
                      valueNoise(vec3(t * 31.0, filament * 8.9 + 70.0, tick))) - 0.5);

    float pinned = pow(sin(PI * clamp(t, 0.0, 1.0)), 0.6);
    float radius = (0.08 + 1.15 * smoothstep(0.0, 0.8, t)) * pinned;

    vec3 tangent = streamTangent(t);
    vec3 normal = normalize(cross(tangent, vec3(0.1, 1.0, 0.3)));
    vec3 binormal = cross(tangent, normal);
    return streamPoint(t) + (normal * o.x + binormal * o.y) * radius;
}

void particleSimulate(inout Particle p, uint id) {
    float filament = float(id % FILAMENTS);
    float speed = 0.22 + 0.10 * filament / float(FILAMENTS);
    float t = fract(rnd(id * 3u + 1u) + frame.time * speed);

    vec3 here = filamentPoint(t, filament);
    vec3 next = filamentPoint(min(t + 0.004, 1.0), filament);

    vec3 scatter = vec3(rnd(id * 5u), rnd(id * 7u), rnd(id * 11u)) - 0.5;
    p.position = vec4(here + scatter * 0.03, 0.0);
    p.velocity = vec4(next - here, 1.0);
    p.a = vec4(t, filament, 0.0, 0.0);
}
)GLSL";

inline constexpr const char* kPlasmaVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    float t = p.a.x;
    float reached = smoothstep(STREAM * 1.05, STREAM * 1.05 - 0.06, t);

    vec3 core = mix(vec3(0.75, 0.88, 1.0), vec3(0.28, 0.40, 1.0), smoothstep(0.0, 0.45, t));
    vec3 tint = mix(core, vec3(0.75, 0.16, 1.0), smoothstep(0.45, 0.95, t));

    float strand = 0.6 + 0.4 * sin(p.a.y * 17.0 + frame.time * 9.0);
    color = vec4(tint * reached * strand * FLICKER * 0.055, 1.0);
    data = vec4(0.0);

    clipPos = stretchedSprite(p.position.xyz, p.velocity.xyz, corner, 0.008, 0.06);
}
)GLSL";

// --- Smoke --------------------------------------------------------------------

inline constexpr const char* kSmokeSimulate = R"GLSL(
void particleSimulate(inout Particle p, uint id) {
    p.position.w += frame.dt;

    if (p.position.w >= p.velocity.w) {
        uint seed = id * 2741u + frame.frameIndex * 6151u;
        // Fixed emitters: particles leaving the same point ride almost the same
        // path through the flow, and those paths are what read as wisps.
        const uint EMITTERS = 60u;
        float t = (float(id % EMITTERS) + 0.5) / float(EMITTERS);
        vec3 scatter = vec3(rnd(seed + 1u), rnd(seed + 2u), rnd(seed + 3u)) - 0.5;
        vec3 origin = streamPoint(t) + scatter * 0.12;
        float alive = step(rnd(seed + 4u), SMOKE);
        p.position = vec4(origin, 0.0);
        p.velocity = vec4(0.0, 0.3, 0.0, mix(3.0, 6.0, rnd(seed + 5u)));
        p.b = vec4(alive, rnd(seed + 6u), rnd(seed + 7u), 0.0);
        return;
    }

    vec3 x = p.position.xyz;
    vec3 flow = curlNoise(x * 0.55 + vec3(0.0, -SECONDS * 0.10, 0.0)) * 1.3;
    vec3 toTail = normalize(params.streamEnd.xyz + vec3(0.0, 1.2, 0.0) - x);
    vec3 v = mix(p.velocity.xyz, flow + toTail * 0.9 + vec3(0.0, 0.25, 0.0),
                 1.0 - exp(-frame.dt * 2.0));
    p.position.xyz = x + v * frame.dt;
    p.velocity.xyz = v;
}
)GLSL";

inline constexpr const char* kSmokeVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    vec3 x = p.position.xyz;
    float life = clamp(p.position.w / max(p.velocity.w, 1e-3), 0.0, 1.0);
    float fade = smoothstep(0.0, 0.2, life) * (1.0 - smoothstep(0.5, 1.0, life));

    float size = mix(0.16, 0.70, sqrt(life)) * (0.7 + 0.6 * p.b.y);

    // There's no depth to read here, but the sand is analytic: dissolving near
    // it hides the hard line where a sprite would cut into the ground.
    float clearance = (x.y - groundHeight(x.xz)) / size;
    float soft = smoothstep(0.1, 1.0, clearance);

    // The smoke is lit only by the effect itself.
    vec3 chest = genieLocal(vec3(0.0, 4.2, 0.0));
    float nearBody = 9.0 / (6.0 + dot(x - chest, x - chest)) * REVEAL * (1.0 - DISSOLVE);
    vec3 mid = streamPoint(0.45);
    float nearStream = 8.0 / (4.0 + dot(x - mid, x - mid)) * STREAM;

    vec3 base = vec3(0.16, 0.035, 0.26);
    vec3 lit = base * (0.20 + 0.35 * FLICKER * nearStream) +
               vec3(0.75, 0.08, 0.60) * nearBody * 0.10 +
               vec3(0.12, 0.22, 0.85) * nearStream * FLICKER * 0.05;

    // Fades with height, so it never builds a wall behind the genie.
    float settle = 1.0 - smoothstep(2.5, 5.5, x.y);
    float puff = 0.25 + 2.2 * p.b.y * p.b.y;
    float alpha = 0.042 * puff * p.b.x * fade * soft * settle * smoothstep(0.0, 0.3, SMOKE);
    color = vec4(lit * (0.6 + 0.8 * p.b.z) * alpha, alpha);

    float angle = p.b.z * 2.0 * PI + life * 0.8;
    float c = cos(angle), s = sin(angle);
    data = vec4(mat2(c, -s, s, c) * corner * 0.8 + p.b.yz * 37.0, 0.0, 0.0);

    vec4 viewPos = frame.view * vec4(x, 1.0);
    viewPos.xy += corner * size;
    clipPos = frame.projection * viewPos;
}
)GLSL";

inline constexpr const char* kSmokeFragment = R"GLSL(
vec4 particleFragment(vec2 corner, vec4 color, vec4 data) {
    float r = 1.0 - dot(corner, corner);
    if (r <= 0.0) discard;
    float wisps = valueNoise(vec3(data.xy, 0.0)) * 0.6 +
                  valueNoise(vec3(data.xy * 2.3, 4.0)) * 0.4;
    return color * r * r * smoothstep(0.32, 0.72, wisps) * 2.2;
}
)GLSL";

// --- Glows --------------------------------------------------------------------

inline constexpr std::uint32_t kGlowCount = 6;

inline constexpr const char* kGlowSimulate = R"GLSL(
void particleSimulate(inout Particle p, uint id) {}
)GLSL";

// Each sprite is one fixed light: the haze a real camera would see around it.
inline constexpr const char* kGlowVertex = R"GLSL(
void particleVertex(Particle p, uint id, vec2 corner,
                    out vec4 clipPos, out vec4 color, out vec4 data) {
    float body = REVEAL * (1.0 - DISSOLVE);
    vec3 pos;
    vec3 tint;
    float size;

    if (id == 0u) {          // Haze around the torso.
        pos = genieLocal(vec3(-0.6, 3.1, 0.0));
        tint = vec3(0.85, 0.10, 0.55) * 0.14 * body;
        size = 6.5;
    } else if (id == 1u) {   // Head.
        pos = genieLocal(vec3(0.0, 6.55, 0.1));
        tint = vec3(0.32, 0.20, 1.0) * 0.40 * smoothstep(0.85, 1.0, REVEAL) * (1.0 - DISSOLVE);
        size = 2.4;
    } else if (id == 2u || id == 3u) {  // Eyes.
        float side = id == 2u ? -1.0 : 1.0;
        pos = genieLocal(vec3(side * 0.25, 6.62, 0.62));
        tint = vec3(0.95, 0.80, 1.0) * 5.5 * EYES;
        size = 0.24;
    } else if (id == 4u) {   // Lamp spout.
        pos = params.spout.xyz;
        tint = vec3(0.35, 0.55, 1.0) * 0.50 * smoothstep(0.0, 0.2, STREAM) * FLICKER;
        size = 1.5;
    } else {                 // Where the stream becomes the genie.
        pos = streamPoint(0.72);
        tint = vec3(0.55, 0.40, 1.0) * 0.22 * smoothstep(0.6, 0.9, STREAM) * FLICKER;
        size = 1.9;
    }

    color = vec4(tint, 1.0);
    data = vec4(0.0);
    vec4 viewPos = frame.view * vec4(pos, 1.0);
    viewPos.xy += corner * size;
    clipPos = frame.projection * viewPos;
}
)GLSL";

inline constexpr const char* kGlowFragment = R"GLSL(
vec4 particleFragment(vec2 corner, vec4 color, vec4 data) {
    float r2 = dot(corner, corner);
    if (r2 >= 1.0) discard;
    float falloff = 1.0 / (1.0 + 24.0 * r2) - 0.04;
    return vec4(color.rgb * max(falloff, 0.0) * (1.0 - r2), 0.0);
}
)GLSL";

// --- Effect descriptions ------------------------------------------------------

inline iron::GpuParticlesDesc makeEffect(std::uint32_t count, iron::ParticleBlend blend,
                                         const char* simulate, const char* vertex,
                                         const char* fragment) {
    iron::GpuParticlesDesc desc;
    desc.count        = count;
    desc.blend        = blend;
    desc.paramsSize   = sizeof(GenieParams);
    desc.commonGlsl   = std::string(kParticleLib) + kCommon;
    desc.simulateGlsl = simulate;
    desc.vertexGlsl   = vertex;
    desc.fragmentGlsl = fragment;
    return desc;
}

inline iron::GpuParticlesDesc makeBodyEffect() {
    return makeEffect(700'000, iron::ParticleBlend::Additive, kBodySimulate, kBodyVertex,
                      kSoftDotFragment);
}

inline iron::GpuParticlesDesc makePlasmaEffect() {
    return makeEffect(160'000, iron::ParticleBlend::Additive, kPlasmaSimulate, kPlasmaVertex,
                      kSoftDotFragment);
}

inline iron::GpuParticlesDesc makeSmokeEffect() {
    return makeEffect(24'000, iron::ParticleBlend::PremultipliedAlpha, kSmokeSimulate,
                      kSmokeVertex, kSmokeFragment);
}

inline iron::GpuParticlesDesc makeGlowEffect() {
    iron::GpuParticlesDesc desc = makeEffect(kGlowCount, iron::ParticleBlend::Additive,
                                             kGlowSimulate, kGlowVertex, kGlowFragment);
    desc.depthTest = false;
    return desc;
}

}  // namespace genie
