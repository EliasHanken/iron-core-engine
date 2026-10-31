#pragma once

// The shaders that make the hall a place: fog that settles and gets stirred, cold
// light falling through cracks in the roof, and shading in the corners. The roof
// cracks appear in the air and on surfaces, so that one light stays in step.

#include <string>

namespace kindle {

// A cold shaft is light falling from a crack in the ceiling, so asking whether a
// point is in one means following the light back up to the ceiling.
inline const char* const kShaftsGlsl = R"GLSL(
const vec3 kShaftColor = vec3(0.42, 0.58, 0.85);
const vec3 kShaftFrom = vec3(0.34, 1.0, 0.18);
const float kCeiling = 6.0;

float crackLine(float v, float width) {
    float d = abs(fract(v) - 0.5);
    return 1.0 - smoothstep(width * 0.1, width, d);
}

float shaftAt(vec3 p) {
    vec2 q = p.xz + kShaftFrom.xz * ((kCeiling - p.y) / kShaftFrom.y);
    // Long cracks, slightly bent, broken into lengths.
    float bend = sin(q.y * 0.21 + 1.3) * 1.1 + sin(q.y * 0.53) * 0.35;
    float line = crackLine((q.x + bend) / 13.0 + 0.31, 0.055);
    float cut = smoothstep(-0.25, 0.35, sin(q.y * 0.44 + floor(q.x / 13.0 + 0.31) * 2.4));
    // Gaps along the crack split a flat sheet of light into separate beams.
    float beams = 0.5 + 0.5 * sin(q.y * 1.7 + sin(q.y * 0.63) * 2.0) * sin(q.y * 3.9 + 1.1);
    return line * cut * beams;
}
)GLSL";

inline const std::string kAirGlsl = std::string(kShaftsGlsl) + R"GLSL(
void airAt(vec3 p, float time, inout float density, inout vec3 light) {
    // A thin cold haze, so that the far end of the hall fades instead of stopping.
    float falling = mix(0.3, 1.0, smoothstep(-6.0, 6.0, p.y));
    light = kShaftColor * (0.04 + shaftAt(p) * 7.0 * falling);
}
)GLSL";

// Adds the hall to a Forge surface: the shafts landing on it, the cracks themselves,
// and the darkening of creases. That last part is what a lightmap would bake, but
// the hall is plain math, so it needs no bake.
inline std::string hallSurfaceHook(std::string forgeHook) {
    const std::string name = "void surfaceMaterial(";
    forgeHook.replace(forgeHook.find(name), name.size(), "void forgeSurface(");
    return forgeHook + kShaftsGlsl + R"GLSL(
float hallDistance(vec3 p) {
    return min(forgeRoom(p, vec3(0.0), vec3(20.0, 6.0, 20.0)), forgePillars(p, 10.0, 0.8));
}

float hallOcclusion(vec3 p, vec3 n) {
    float occlusion = 0.0;
    float weight = 1.0;
    for (int i = 1; i <= 5; ++i) {
        float reach = float(i) * 0.55;
        occlusion += (reach - hallDistance(p + n * reach)) * weight / reach;
        weight *= 0.7;
    }
    return clamp(1.0 - occlusion * 0.42, 0.0, 1.0);
}

void surfaceMaterial(vec3 worldPos, vec3 N, vec3 V, float surfaceTime, inout vec3 albedo,
                     inout float roughness, inout float metallic, inout vec3 emissive) {
    forgeSurface(worldPos, N, V, surfaceTime, albedo, roughness, metallic, emissive);

    float crease = hallOcclusion(worldPos, N);
    float ceiling = smoothstep(5.95, 6.0, worldPos.y);
    float facing = max(dot(N, normalize(kShaftFrom)), 0.0);
    vec3 surfaceLight = kShaftColor * (shaftAt(worldPos) * facing * 1.6 + 0.012);
    emissive += albedo * surfaceLight * crease;
    emissive += kShaftColor * shaftAt(worldPos) * ceiling * 5.0;
    albedo *= crease;
}
)GLSL";
}

inline const char* const kFogCommonGlsl = R"GLSL(
layout(set = 0, binding = 2) uniform Params {
    vec4 swarm;     // w = stirring radius
    vec4 velocity;
    vec4 kick;      // x = blast
} params;
)GLSL";

inline const char* const kFogSourceGlsl = R"GLSL(
void fluidSource(vec3 world, inout vec3 velocity, inout float density, inout float heat) {
    // Mist keeps settling back along the floor, in slow drifting banks.
    float low = 1.0 - smoothstep(-5.8, -1.6, world.y);
    float banks = 0.5 + 0.5 * sin(world.x * 0.23 + fluid.time * 0.07) *
                              sin(world.z * 0.19 - fluid.time * 0.05);
    float want = low * mix(0.06, 0.8, banks * banks) + 0.01;
    density += (want - density) * min(fluid.dt * 0.35, 1.0);

    // The swarm drags the air along behind it.
    vec3 away = world - params.swarm.xyz;
    float near = exp(-dot(away, away) / (params.swarm.w * params.swarm.w));
    velocity += (params.velocity.xyz * 1.6 - velocity) * near * min(fluid.dt * 7.0, 1.0);

    // A burst blows a hole in the fog, which then rolls back in.
    float reach = exp(-dot(away, away) / 36.0);
    velocity += normalize(away + vec3(1e-4)) * params.kick.x * reach * 90.0 * fluid.dt;

    velocity.y -= density * 0.6 * fluid.dt;
}
)GLSL";

inline const char* const kFogObstacleGlsl = R"GLSL(
float fluidObstacle(vec3 world) {
    vec2 cell = mod(world.xz, 10.0) - 5.0;
    vec2 d = abs(cell) - 0.8;
    return max(d.x, d.y);
}
)GLSL";

}  // namespace kindle
