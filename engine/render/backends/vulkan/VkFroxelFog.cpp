#include "render/backends/vulkan/VkFroxelFog.h"

#include "core/Log.h"
#include "math/Transform.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

namespace iron {

namespace {

struct GpuParams {
    Mat4  cameraToWorld;
    Mat4  previousViewProjection;
    Mat4  cascade[4];
    float cascadeEnds[4];
    float cascadeRadii[4];
    float cascadeInfo[4];  // x on, y caster reach, z disturbances
    float grid[4];         // x, y, z froxels; w = log(far / near)
    float range[4];        // x near, y far, z tan of half the horizontal field, w vertical
    float fog[4];          // x density, y height falloff, z base height, w anisotropy
    float albedo[4];       // rgb; w noise
    float ambient[4];      // rgb; w noise scale
    float wind[4];         // xyz; w time
    float sun[4];          // xyz the way it travels; w = history usable
    float sunColor[4];     // rgb; w = fog boxes
    float camera[4];       // xyz; w = this frame's jitter along depth, 0..1
    float fluid[8][4];     // Per fluid: min (w = density scale, 0 off), then 1 / size (w = albedo)
    float heat[8][4];      // Hot air: centre (w = radius), with strength in heatStrength
    float heatStrength[2][4];
    float heatCount[4];    // x heat sources
};

struct GpuBox {
    float min[4];  // w = density
    float max[4];  // w = edge
};

struct GpuDisturbance {
    float a[4];  // w = radius
    float b[4];  // w = 1 / (softness * radius)
    float k[4];  // x clear, y add, z albedo
};

#define FOG_COMMON R"GLSL(
layout(std140, binding = 0) uniform FogParams {
    mat4 cameraToWorld;
    mat4 previousViewProjection;
    mat4 cascade[4];
    vec4 cascadeEnds;
    vec4 cascadeRadii;
    vec4 cascadeInfo;
    vec4 grid;
    vec4 range;
    vec4 fog;
    vec4 albedo;
    vec4 ambient;
    vec4 wind;
    vec4 sun;
    vec4 sunColor;
    vec4 camera;
    vec4 fluid[8];
    vec4 heat[8];
    vec4 heatStrength[2];
    vec4 heatCount;
} p;
float sliceDepth(float z) { return p.range.x * exp(z / p.grid.z * p.grid.w); }
)GLSL"

const char* const kLight = "#version 450\nlayout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;\n" FOG_COMMON R"GLSL(
layout(binding = 1) uniform sampler2D uShadow;
layout(std140, binding = 2) uniform Clusters { mat4 view; vec4 proj; vec4 grid; vec4 counts; vec4 screen; } cl;
struct Light { vec4 positionRange; vec4 colorIntensity; vec4 spot; vec4 spot2; };
layout(std430, binding = 3) readonly buffer Lights { Light lights[]; };
layout(std430, binding = 4) readonly buffer Cells { uvec4 cells[]; };
layout(std430, binding = 5) readonly buffer Items { uint items[]; };
struct Box { vec4 lo; vec4 hi; };
struct Disturbance { vec4 a; vec4 b; vec4 k; };
layout(std430, binding = 6) readonly buffer Shapes { Box boxes[32]; Disturbance disturbances[]; };
layout(binding = 7) uniform sampler3D uHistory;
layout(rgba16f, binding = 8) writeonly uniform image3D uOut;
layout(rgba16f, binding = 9) writeonly uniform image3D uSmokeLight;

const float PI = 3.14159265;

// The world position at a point in the grid: x and y across the screen (rows
// from the top), z along the slices.
vec3 froxelWorld(vec3 f) {
    vec2 ndc = vec2(f.x / p.grid.x * 2.0 - 1.0, 1.0 - f.y / p.grid.y * 2.0);
    float d = sliceDepth(f.z);
    return (p.cameraToWorld * vec4(ndc.x * p.range.z * d, ndc.y * p.range.w * d, -d, 1.0)).xyz;
}

float hash(vec3 q) {
    q = fract(q * 0.3183099 + vec3(0.1, 0.2, 0.3));
    q *= 17.0;
    return fract(q.x * q.y * q.z * (q.x + q.y + q.z));
}
float valueNoise(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash(i), hash(i + vec3(1, 0, 0)), f.x), mix(hash(i + vec3(0, 1, 0)), hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash(i + vec3(0, 0, 1)), hash(i + vec3(1, 0, 1)), f.x), mix(hash(i + vec3(0, 1, 1)), hash(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}
float fbm(vec3 x) { return 0.55 * valueNoise(x) + 0.3 * valueNoise(x * 2.03) + 0.15 * valueNoise(x * 4.07); }

// How much light a metre of air here stops (x), how much of that it scatters
// rather than absorbs (y, before the fog's albedo), and how much of it is smoke
// (z: what disturbances and fluids added).
vec3 density(vec3 w) {
    float d = p.fog.x * exp(-max(w.y - p.fog.z, 0.0) * p.fog.y);
    int boxCount = int(p.sunColor.w);
    for (int i = 0; i < boxCount; ++i) {
        vec3 inside = min(w - boxes[i].lo.xyz, boxes[i].hi.xyz - w);
        float e = min(min(inside.x, inside.y), inside.z);
        if (e > 0.0) d += boxes[i].lo.w * clamp(e / max(boxes[i].hi.w, 1e-3), 0.0, 1.0);
    }
    // Disturbances: smoke added before the wisps (so it's patchy too), what's
    // pushed aside taken out after (so a wake through smoke clears it as well).
    float keep = 1.0;
    float scatter = d;
    float smoke = 0.0;
    int disturbanceCount = int(p.cascadeInfo.z);
    for (int i = 0; i < disturbanceCount; ++i) {
        vec3 a = disturbances[i].a.xyz;
        vec3 ab = disturbances[i].b.xyz - a;
        float t = clamp(dot(w - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        float r = length(w - a - ab * t);
        float k = clamp((disturbances[i].a.w - r) * disturbances[i].b.w, 0.0, 1.0);
        k = k * k * (3.0 - 2.0 * k);
        d += k * disturbances[i].k.y;
        scatter += k * disturbances[i].k.y * disturbances[i].k.z;
        smoke += k * disturbances[i].k.y;
        keep *= 1.0 - k * disturbances[i].k.x;
    }
    if (p.albedo.w > 0.0) {
        float n = fbm(w * p.ambient.w - p.wind.xyz * p.wind.w * p.ambient.w);
        float wisps = mix(1.0, clamp(n * 2.2 - 0.4, 0.0, 2.0), p.albedo.w);
        d *= wisps;
        scatter *= wisps;
        smoke *= wisps;
    }
    return vec3(d, scatter, smoke) * keep;
}

float sunShadow(vec3 w, float dist) {
    if (p.cascadeInfo.x < 0.5) return 1.0;
    int c = 0;
    for (; c < 4; ++c) {
        if (dist < p.cascadeEnds[c]) break;
    }
    if (c >= 4) return 1.0;
    vec4 lp = p.cascade[c] * vec4(w, 1.0);
    vec3 pr = lp.xyz / lp.w;
    if (abs(pr.x) > 1.0 || abs(pr.y) > 1.0 || pr.z > 1.0) return 1.0;
    vec2 uv = (pr.xy * 0.5 + 0.5) * 0.5 + vec2(float(c % 2), float(c / 2)) * 0.5;
    return pr.z - 0.002 > textureLod(uShadow, uv, 0.0).r ? 0.0 : 1.0;
}

// Henyey-Greenstein: how much light turning by this angle scatters towards the eye.
float phase(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

void main() {
    uvec3 id = gl_GlobalInvocationID;
    if (id.x >= uint(p.grid.x) || id.y >= uint(p.grid.y) || id.z >= uint(p.grid.z)) return;
    vec3 w = froxelWorld(vec3(id) + vec3(0.5, 0.5, p.camera.w));
    vec3 sigma = density(w);
    float sigmaT = sigma.x;
    // Thick smoke scatters light many times over, so it glows white from every side
    // rather than only towards the light like thin haze: where it's smoke, the
    // phase evens out and the sky lights it more (a cheap stand-in for multiple
    // scattering).
    float smokeShare = clamp(sigma.z / max(sigmaT, 1e-5), 0.0, 1.0);
    float smokeG = mix(p.fog.w, 0.15, smokeShare);
    vec3 toEye = p.camera.xyz - w;
    float dist = length(toEye);
    toEye /= max(dist, 1e-4);

    // The same light twice: as this cell's air scatters it, and as smoke would
    // (nearly even, lit more by the sky) for the smoke march.
    const float kSmokeG = 0.15;
    vec3 sunLight = p.sunColor.rgb * sunShadow(w, dist);
    float sunCos = dot(p.sun.xyz, toEye);
    vec3 radiance = sunLight * phase(sunCos, smokeG);
    vec3 smokeLight = sunLight * phase(sunCos, kSmokeG);
    // Lights through the clusters: this cell's tile and slice in their grid.
    vec2 tile = (vec2(id.xy) + 0.5) / p.grid.xy * cl.grid.xy;
    float d = sliceDepth(float(id.z) + 0.5);
    float slice = log(max(d, cl.proj.z) / cl.proj.z) / cl.counts.z * cl.grid.z;
    ivec3 g = clamp(ivec3(int(tile.x), int(tile.y), int(slice)), ivec3(0), ivec3(cl.grid.xyz) - 1);
    uint cluster = uint(g.x) + uint(g.y) * uint(cl.grid.x) + uint(g.z) * uint(cl.grid.x * cl.grid.y);
    uint count = cells[cluster].x;
    for (uint k = 0u; k < count; ++k) {
        Light l = lights[items[cluster * 96u + k]];
        vec3 fromLight = w - l.positionRange.xyz;
        float ld = length(fromLight);
        float range = l.positionRange.w;
        if (ld >= range || ld < 1e-3) continue;
        fromLight /= ld;
        float falloff = 1.0 - smoothstep(0.0, range, ld);
        if (l.spot.w > -1.5) falloff *= smoothstep(l.spot.w, max(l.spot2.x, l.spot.w + 1e-4), dot(fromLight, l.spot.xyz));
        vec3 lit = l.colorIntensity.rgb * l.colorIntensity.w * falloff;
        float c = dot(fromLight, toEye);
        radiance += lit * phase(c, smokeG);
        smokeLight += lit * phase(c, kSmokeG);
    }
    radiance += p.ambient.rgb * mix(1.0, 5.0, smokeShare);
    imageStore(uSmokeLight, ivec3(id), vec4(smokeLight + p.ambient.rgb * 5.0, 0.0));
    vec4 current = vec4(sigma.y * p.albedo.rgb * radiance, sigmaT);

    // Last frame's grid at this cell's centre, where it was then.
    if (p.sun.w > 0.5) {
        vec3 centre = froxelWorld(vec3(id) + 0.5);
        vec4 prev = p.previousViewProjection * vec4(centre, 1.0);
        if (prev.w > 0.0) {
            vec3 ndc = prev.xyz / prev.w;
            vec3 uvw = vec3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5,
                            log(max(prev.w, p.range.x) / p.range.x) / p.grid.w);
            if (all(greaterThanEqual(uvw, vec3(0.0))) && all(lessThanEqual(uvw, vec3(1.0)))) {
                current = mix(current, textureLod(uHistory, uvw, 0.0), 0.88);
            }
        }
    }
    imageStore(uOut, ivec3(id), current);
}
)GLSL";

const char* const kIntegrate = "#version 450\nlayout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;\n" FOG_COMMON R"GLSL(
layout(rgba16f, binding = 1) readonly uniform image3D uIn;
layout(rgba16f, binding = 2) writeonly uniform image3D uOut;

// Front to back along each ray: what it has gathered so far, and how much of the
// light behind still gets through. Each slice's own scattering is integrated
// across its thickness (Frostbite's energy-conserving form).
void main() {
    ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (xy.x >= int(p.grid.x) || xy.y >= int(p.grid.y)) return;
    vec3 gathered = vec3(0.0);
    float through = 1.0;
    int slices = int(p.grid.z);
    for (int z = 0; z < slices; ++z) {
        vec4 s = imageLoad(uIn, ivec3(xy, z));
        float thickness = sliceDepth(float(z + 1)) - sliceDepth(float(z));
        float sigma = max(s.a, 1e-6);
        float sliceThrough = exp(-sigma * thickness);
        gathered += through * (s.rgb - s.rgb * sliceThrough) / sigma;
        through *= sliceThrough;
        imageStore(uOut, ivec3(xy, z), vec4(gathered, through));
    }
}
)GLSL";

const char* const kSmoke = "#version 450\nlayout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;\n" FOG_COMMON R"GLSL(
layout(binding = 1) uniform sampler2D uDepth;
layout(binding = 2) uniform sampler3D uAir;     // Integrated: gathered light, and what gets through.
layout(binding = 3) uniform sampler3D uLight;   // The light smoke scatters at each cell.
layout(binding = 4) uniform sampler3D uFluid0;
layout(binding = 5) uniform sampler3D uFluid1;
layout(binding = 6) uniform sampler3D uFluid2;
layout(binding = 9) uniform sampler3D uFluid3;
layout(rgba16f, binding = 7) writeonly uniform image2D uOut;
layout(r16f, binding = 8) writeonly uniform image2D uShimmer;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;  // a: depthA, depthB, width, height; b: frame

float hash3(vec3 q) {
    q = fract(q * 0.3183099 + vec3(0.1, 0.2, 0.3));
    q *= 17.0;
    return fract(q.x * q.y * q.z * (q.x + q.y + q.z));
}
float noise3(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i), hash3(i + vec3(1, 0, 0)), f.x), mix(hash3(i + vec3(0, 1, 0)), hash3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0, 0, 1)), hash3(i + vec3(1, 0, 1)), f.x), mix(hash3(i + vec3(0, 1, 1)), hash3(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}

// Where a world point sits in the froxel grid (0..1 each way).
vec3 gridAt(vec3 w) {
    vec3 rel = w - p.cameraToWorld[3].xyz;
    float d = max(-dot(rel, p.cameraToWorld[2].xyz), p.range.x);
    vec2 ndc = vec2(dot(rel, p.cameraToWorld[0].xyz) / (d * p.range.z), dot(rel, p.cameraToWorld[1].xyz) / (d * p.range.w));
    return vec3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, log(d / p.range.x) / p.grid.w);
}

// One fluid's density (r) and heat (g) at a world point, 0 outside it.
vec2 fluidAt(int i, vec3 w) {
    vec4 lo = p.fluid[i * 2];
    if (lo.w <= 0.0) return vec2(0.0);
    vec3 uvw = (w - lo.xyz) * p.fluid[i * 2 + 1].xyz;
    if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0)))) return vec2(0.0);
    vec2 raw = i == 0 ? textureLod(uFluid0, uvw, 0.0).rg
             : i == 1 ? textureLod(uFluid1, uvw, 0.0).rg
             : i == 2 ? textureLod(uFluid2, uvw, 0.0).rg : textureLod(uFluid3, uvw, 0.0).rg;
    return vec2(max(raw.r, 0.0) * lo.w, max(raw.g, 0.0));
}

// Every fluid's smoke at a point, as extinction per metre.
float smokeAt(vec3 w) {
    return fluidAt(0, w).x + fluidAt(1, w).x + fluidAt(2, w).x + fluidAt(3, w).x;
}

vec2 fluidSlab(int i, vec3 o, vec3 dir) {
    vec3 lo = p.fluid[i * 2].xyz;
    vec3 hi = lo + 1.0 / p.fluid[i * 2 + 1].xyz;
    vec3 inv = 1.0 / dir;
    vec3 t0 = (lo - o) * inv;
    vec3 t1 = (hi - o) * inv;
    vec3 near = min(t0, t1);
    vec3 far = max(t0, t1);
    return vec2(max(max(near.x, near.y), near.z), min(min(far.x, far.y), far.z));
}

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(pc.a.z) || px.y >= int(pc.a.w)) return;
    // The view ray through this pixel (row 0 at the top), and how far the scene is.
    vec2 ndc = vec2((float(px.x) + 0.5) / pc.a.z * 2.0 - 1.0, 1.0 - (float(px.y) + 0.5) / pc.a.w * 2.0);
    vec3 viewDir = normalize(vec3(ndc.x * p.range.z, ndc.y * p.range.w, -1.0));
    vec3 dir = mat3(p.cameraToWorld) * viewDir;
    vec3 o = p.cameraToWorld[3].xyz;
    float z = texelFetch(uDepth, px, 0).r;
    float sceneDepth = z >= 1.0 ? 1e4 : pc.a.y / (z + pc.a.x);
    float tScene = sceneDepth / max(-viewDir.z, 1e-4);

    // The span of the ray inside any fluid.
    float t0 = 1e9, t1 = -1e9;
    for (int i = 0; i < 4; ++i) {
        if (p.fluid[i * 2].w <= 0.0) continue;
        vec2 s = fluidSlab(i, o, dir);
        if (s.y > max(s.x, 0.0)) {
            t0 = min(t0, max(s.x, 0.0));
            t1 = max(t1, s.y);
        }
    }
    t1 = min(t1, tScene);

    // Hot air on the way to the scene: each heat source by how much of it the ray
    // crosses, strongest through the middle.
    float shimmer = 0.0;
    int heats = int(p.heatCount.x);
    for (int i = 0; i < heats; ++i) {
        vec3 c = p.heat[i].xyz - o;
        float r = p.heat[i].w;
        float along = dot(c, dir);
        float miss2 = dot(c, c) - along * along;
        if (miss2 >= r * r || along - r > tScene || along + r < 0.0) continue;
        float chord = sqrt(r * r - miss2);
        float crossed = clamp(min(along + chord, tScene) - max(along - chord, 0.0), 0.0, 2.0 * chord);
        shimmer += p.heatStrength[i / 4][i % 4] * crossed / (2.0 * r) * (1.0 - miss2 / (r * r));
    }

    if (t1 <= t0) {
        imageStore(uOut, px, vec4(0.0, 0.0, 0.0, 1.0));
        imageStore(uShimmer, px, vec4(shimmer, 0.0, 0.0, 0.0));
        return;
    }

    // March it in fixed fine steps through smoke, striding over empty air and
    // backing up to fine steps on the way in, so every ray samples smoke the same
    // however far the scene behind it is. A different offset each pixel and frame
    // (TAA gathers them).
    const float kFine = 0.25;
    const float kStride = 1.0;
    float jitter = fract(52.9829189 * fract(dot(vec2(px) + pc.b.x * 5.588238, vec2(0.06711056, 0.00583715))));
    vec3 gathered = vec3(0.0);
    float through = 1.0;
    float t = t0 + jitter * kFine;
    bool striding = false;
    for (int k = 0; k < 220 && t < t1; ++k) {
        vec3 w = o + dir * t;
        float dt = kFine;
        float sigma = 0.0;
        float scatter = 0.0;
        float heat = 0.0;
        for (int i = 0; i < 4; ++i) {
            vec2 f = fluidAt(i, w);
            heat += f.y;
            sigma += f.x;
            scatter += f.x * p.fluid[i * 2 + 1].w;
        }
        if (sigma < 1e-3) {
            // Empty air: stride on (heat still bends the light across the stride).
            shimmer += heat * kStride * through * 0.12;
            t += kStride;
            striding = true;
            continue;
        }
        if (striding) {
            // Just struck smoke from a stride: back up and come in finely.
            t -= kStride - kFine;
            striding = false;
            continue;
        }
        // Heat bends the light through it, unless smoke in front hides it.
        shimmer += heat * dt * through * 0.12;
        t += dt;
        // Billows: the thin outer smoke is eaten away by drifting noise finer
        // than the fluid's cells; the thick core is left whole.
        // Finer noise all through hides the fluid's cells: they'd show as soft
        // blocks up close.
        vec3 drift = w - p.wind.xyz * p.wind.w * 0.5;
        float n = 0.5 * noise3(drift * 1.7) + 0.3 * noise3(drift * 4.3 + 7.1) + 0.2 * noise3(drift * 9.7 + 3.3);
        float edge = 1.0 - smoothstep(0.05, 0.9, sigma);
        float eaten = max(1.0 - (1.0 - n) * 1.3 * edge, 0.0) * (0.55 + 0.9 * n);
        sigma *= eaten;
        scatter *= eaten;
        if (sigma < 1e-3) continue;
        vec3 g = gridAt(w);
        vec3 light = textureLod(uLight, g, 0.0).rgb;
        // Smoke shades itself: the sun reaches in only through the smoke between
        // (four growing steps towards it), and less sky reaches what's under more
        // smoke. Dark bellies, bright sunlit crowns: what gives a cloud its volume.
        {
            vec3 toSun = -p.sun.xyz;
            float depth = 0.0;
            float stride = 0.3;
            vec3 q = w;
            for (int k = 0; k < 4; ++k) {
                q += toSun * stride;
                depth += smokeAt(q) * stride;
                stride *= 1.9;
            }
            float sunThrough = exp(-depth * 0.8);
            float skyThrough = exp(-smokeAt(w + vec3(0.0, 0.9, 0.0)) * 0.6);
            light *= mix(0.22, 1.0, sunThrough) * mix(0.6, 1.0, skyThrough);
        }
        float airThrough = textureLod(uAir, g, 0.0).a;   // Fog between the eye and here.
        vec3 s = scatter * p.albedo.rgb * light * airThrough;
        float stepThrough = exp(-sigma * dt);
        gathered += through * (s - s * stepThrough) / sigma;
        through *= stepThrough;
        if (through < 0.01) break;
    }
    imageStore(uOut, px, vec4(gathered, through));
    imageStore(uShimmer, px, vec4(shimmer, 0.0, 0.0, 0.0));
}
)GLSL";

const char* const kApplyVert = R"GLSL(#version 450
void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char* const kApplyFrag = R"GLSL(#version 450
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler3D uFog;
layout(binding = 1) uniform sampler2D uDepth;
layout(binding = 2) uniform sampler2D uSmoke;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;  // a: depthA, depthB, near, log(far/near); b: slices, far, smoke
// Blended over the scene as color * through + gathered.
void main() {
    vec2 size = vec2(textureSize(uDepth, 0));
    vec2 uv = gl_FragCoord.xy / size;
    float ndc = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
    float d = ndc >= 1.0 ? pc.b.y : pc.a.y / (ndc + pc.a.x);
    float slices = pc.b.x;
    // The integrated value at slice k reaches the far edge of k: shift half a slice.
    float w = (log(max(d, pc.a.z) / pc.a.z) / pc.a.w * slices - 0.5) / slices;
    vec4 fog = textureLod(uFog, vec3(uv, clamp(w, 0.5 / slices, 1.0)), 0.0);
    // Nearer than the first slice, the fog fades in from nothing.
    float nearFade = clamp(d / max(pc.a.z * 2.0, 1e-3), 0.0, 1.0);
    vec4 air = vec4(fog.rgb * nearFade, mix(1.0, fog.a, nearFade));
    // Smoke in front of it all: its light, then the air and scene through it.
    if (pc.b.z > 0.5) {
        vec4 smoke = texelFetch(uSmoke, ivec2(gl_FragCoord.xy), 0);
        air = vec4(smoke.rgb + smoke.a * air.rgb, smoke.a * air.a);
    }
    outColor = air;
}
)GLSL";

VkShaderModule moduleFor(VkDevice device, VkShaderStageFlagBits stage, const char* source) {
    const auto spirv = compileGlsl(stage, source);
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(std::uint32_t);
    info.pCode    = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

}  // namespace

bool VkFroxelFog::init(VkContext& ctx, VkFormat sceneFormat) {
    sceneFormat_ = sceneFormat;
    const VkDevice device = ctx.device();
    using T = VkDescriptorType;
    if (!lightStage_.init(ctx, kLight,
                          {T::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, T::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           T::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, T::VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           T::VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, T::VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           T::VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, T::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           T::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, T::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}) ||
        !smokeStage_.init(ctx, kSmoke,
                          {T::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, T::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           T::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, T::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           T::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, T::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           T::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, T::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           T::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, T::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                          sizeof(float) * 8) ||
        !integrateStage_.init(ctx, kIntegrate,
                              {T::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, T::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               T::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE})) {
        Log::error("VkFroxelFog: compute shaders failed");
        return false;
    }
    const VolumeSize size{kX, kY, kZ};
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (auto& s : scattering_) {
        if (!s.create(ctx, size, VK_FORMAT_R16G16B16A16_SFLOAT, usage)) return false;
    }
    if (!integrated_.create(ctx, size, VK_FORMAT_R16G16B16A16_SFLOAT, usage)) return false;
    if (!smokeLight_.create(ctx, size, VK_FORMAT_R16G16B16A16_SFLOAT, usage)) return false;

    for (std::size_t i = 0; i < params_.size(); ++i) {
        for (auto [buffer, bytes, use] : {std::tuple{&params_[i], VkDeviceSize{sizeof(GpuParams)},
                                                     VkBufferUsageFlags{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT}},
                                          std::tuple{&volumes_[i], VkDeviceSize{sizeof(GpuBox) * kMaxVolumes +
                                                                              sizeof(GpuDisturbance) * kMaxDisturbances},
                                                     VkBufferUsageFlags{VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}}}) {
            VkBufferCreateInfo bi{};
            bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size        = bytes;
            bi.usage       = use;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &buffer->buffer, &buffer->alloc, &info));
            buffer->mapped = info.pMappedData;
        }
    }

    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device, &si, nullptr, &linear_));
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(device, &si, nullptr, &nearest_));

    // The apply pass: blends into the scene image, which stays sampleable.
    VkAttachmentDescription color{};
    color.format         = sceneFormat;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    color.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_SHADER_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_SHADER_READ_BIT;
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{};
    rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments    = &color;
    rp.subpassCount    = 1;
    rp.pSubpasses      = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies   = deps;
    VK_CHECK(vkCreateRenderPass(device, &rp, nullptr, &applyPass_));

    VkDescriptorSetLayoutBinding b[3]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings    = b;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &li, nullptr, &applySetLayout_));
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets       = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(device, &pi, nullptr, &applyPool_));
    VkDescriptorSetAllocateInfo dai{};
    dai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool     = applyPool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts        = &applySetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(device, &dai, &applySet_));
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount         = 1;
    pl.pSetLayouts            = &applySetLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &push;
    VK_CHECK(vkCreatePipelineLayout(device, &pl, nullptr, &applyLayout_));

    const VkShaderModule vert = moduleFor(device, VK_SHADER_STAGE_VERTEX_BIT, kApplyVert);
    const VkShaderModule frag = moduleFor(device, VK_SHADER_STAGE_FRAGMENT_BIT, kApplyFrag);
    if (!vert || !frag) {
        Log::error("VkFroxelFog: apply shaders failed");
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert,
                 "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag,
                 "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;        // + gathered
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;  // scene * through
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &blend;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dyn;
    VkGraphicsPipelineCreateInfo gp{};
    gp.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount          = 2;
    gp.pStages             = stages;
    gp.pVertexInputState   = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState      = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState   = &ms;
    gp.pDepthStencilState  = &ds;
    gp.pColorBlendState    = &cb;
    gp.pDynamicState       = &dy;
    gp.layout              = applyLayout_;
    gp.renderPass          = applyPass_;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &applyPipeline_));
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    return applyPipeline_ != VK_NULL_HANDLE;
}

bool VkFroxelFog::createTargets(VkContext& ctx, VkExtent2D extent, VkImageView sceneColor, VkImageView sceneDepth) {
    extent_ = extent;
    sceneDepth_ = sceneDepth;
    VkFramebufferCreateInfo fb{};
    fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass      = applyPass_;
    fb.attachmentCount = 1;
    fb.pAttachments    = &sceneColor;
    fb.width           = extent.width;
    fb.height          = extent.height;
    fb.layers          = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device(), &fb, nullptr, &applyFramebuffer_));
    if (!smoke_.create2D(ctx, extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !shimmer_.create2D(ctx, extent.width, extent.height, VK_FORMAT_R16_SFLOAT,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        return false;
    }
    smokeReady_ = false;
    const VkDescriptorImageInfo images[3] = {{linear_, integrated_.view(), VK_IMAGE_LAYOUT_GENERAL},
                                             {nearest_, sceneDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                                             {nearest_, smoke_.view(), VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet          = applySet_;
        w[i].dstBinding      = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo      = &images[i];
    }
    vkUpdateDescriptorSets(ctx.device(), 3, w, 0, nullptr);
    historyValid_ = false;
    return true;
}

void VkFroxelFog::destroyTargets(VkContext& ctx) {
    if (applyFramebuffer_) vkDestroyFramebuffer(ctx.device(), applyFramebuffer_, nullptr);
    applyFramebuffer_ = VK_NULL_HANDLE;
    smoke_.destroy(ctx);
    shimmer_.destroy(ctx);
    smokeReady_ = false;
}

void VkFroxelFog::record(VulkanRenderer& renderer, VkCommandBuffer cb, const Frame& f) {
    const std::size_t slot = f.frameIndex % params_.size();
    GpuParams p{};
    p.cameraToWorld = inverse(f.view);
    p.previousViewProjection = f.previousViewProjection;
    if (f.cascades) {
        for (int i = 0; i < 4; ++i) p.cascade[i] = f.cascades->viewProj[static_cast<std::size_t>(i)];
        for (int i = 0; i < 4; ++i) {
            p.cascadeEnds[i] = f.cascades->ends[static_cast<std::size_t>(i)];
            p.cascadeRadii[i] = f.cascades->radii[static_cast<std::size_t>(i)];
        }
        p.cascadeInfo[0] = 1.0f;
        p.cascadeInfo[1] = f.casterReach;
    }
    const float far = std::max(f.fog.distance, kNear * 4.0f);
    p.grid[0] = static_cast<float>(kX);
    p.grid[1] = static_cast<float>(kY);
    p.grid[2] = static_cast<float>(kZ);
    p.grid[3] = std::log(far / kNear);
    p.range[0] = kNear;
    p.range[1] = far;
    p.range[2] = 1.0f / f.projection.at(0, 0);
    p.range[3] = 1.0f / f.projection.at(1, 1);
    p.fog[0] = f.fog.density;
    p.fog[1] = f.fog.heightFalloff;
    p.fog[2] = f.fog.baseHeight;
    p.fog[3] = std::clamp(f.fog.anisotropy, -0.95f, 0.95f);
    p.albedo[0] = f.fog.albedo.x;
    p.albedo[1] = f.fog.albedo.y;
    p.albedo[2] = f.fog.albedo.z;
    p.albedo[3] = std::clamp(f.fog.noise, 0.0f, 1.0f);
    p.ambient[0] = f.fog.ambient.x;
    p.ambient[1] = f.fog.ambient.y;
    p.ambient[2] = f.fog.ambient.z;
    p.ambient[3] = f.fog.noiseScale;
    p.wind[0] = f.fog.wind.x;
    p.wind[1] = f.fog.wind.y;
    p.wind[2] = f.fog.wind.z;
    p.wind[3] = f.time;
    const Vec3 sun = normalize(f.sunDirection);
    p.sun[0] = sun.x;
    p.sun[1] = sun.y;
    p.sun[2] = sun.z;
    p.sun[3] = (historyValid_ && f.hasPrevious) ? 1.0f : 0.0f;
    p.sunColor[0] = f.sunColor.x;
    p.sunColor[1] = f.sunColor.y;
    p.sunColor[2] = f.sunColor.z;
    const std::size_t boxes = std::min<std::size_t>(f.volumes.size(), kMaxVolumes);
    p.sunColor[3] = static_cast<float>(boxes);
    for (std::size_t i = 0; i < std::min<std::size_t>(f.fluidCount, f.fluids.size()); ++i) {
        const Frame::Fluid& fl = f.fluids[i];
        const Vec3 size = fl.bounds.max - fl.bounds.min;
        float* lo = p.fluid[i * 2];
        float* inv = p.fluid[i * 2 + 1];
        lo[0] = fl.bounds.min.x;
        lo[1] = fl.bounds.min.y;
        lo[2] = fl.bounds.min.z;
        lo[3] = fl.density != VK_NULL_HANDLE ? std::max(fl.scale, 0.0f) : 0.0f;
        inv[0] = 1.0f / std::max(size.x, 1e-3f);
        inv[1] = 1.0f / std::max(size.y, 1e-3f);
        inv[2] = 1.0f / std::max(size.z, 1e-3f);
        inv[3] = std::clamp(fl.albedo, 0.0f, 1.0f);
    }
    const std::size_t heats = std::min<std::size_t>(f.heat.size(), kMaxHeat);
    for (std::size_t i = 0; i < heats; ++i) {
        const HeatSource& h = f.heat[i];
        p.heat[i][0] = h.centre.x;
        p.heat[i][1] = h.centre.y;
        p.heat[i][2] = h.centre.z;
        p.heat[i][3] = std::max(h.radius, 1e-3f);
        p.heatStrength[i / 4][i % 4] = std::max(h.strength, 0.0f);
    }
    p.heatCount[0] = static_cast<float>(heats);
    const std::size_t disturbances = std::min<std::size_t>(f.disturbances.size(), kMaxDisturbances);
    p.cascadeInfo[2] = static_cast<float>(disturbances);
    p.camera[0] = f.cameraPos.x;
    p.camera[1] = f.cameraPos.y;
    p.camera[2] = f.cameraPos.z;
    // A different depth within each cell every frame (the base-2 van der Corput
    // sequence), so the temporal blend gathers the whole cell.
    std::uint32_t n = (f.frameIndex % 16u) + 1u;
    float jitter = 0.0f, scale = 0.5f;
    while (n > 0) {
        jitter += static_cast<float>(n & 1u) * scale;
        n >>= 1u;
        scale *= 0.5f;
    }
    p.camera[3] = jitter;
    std::memcpy(params_[slot].mapped, &p, sizeof(p));
    auto* box = static_cast<GpuBox*>(volumes_[slot].mapped);
    for (std::size_t i = 0; i < boxes; ++i) {
        const FogVolume& v = f.volumes[i];
        box[i] = GpuBox{{v.min.x, v.min.y, v.min.z, v.density}, {v.max.x, v.max.y, v.max.z, v.edge}};
    }
    auto* disturbance = reinterpret_cast<GpuDisturbance*>(box + kMaxVolumes);
    for (std::size_t i = 0; i < disturbances; ++i) {
        const FogDisturbance& d = f.disturbances[i];
        const float r = std::max(d.radius, 1e-3f);
        disturbance[i] = GpuDisturbance{{d.a.x, d.a.y, d.a.z, r},
                                        {d.b.x, d.b.y, d.b.z, 1.0f / (std::clamp(d.softness, 0.02f, 1.0f) * r)},
                                        {std::clamp(d.clear, 0.0f, 1.0f), std::max(d.add, 0.0f), std::clamp(d.albedo, 0.0f, 1.0f), 0.0f}};
    }

    VkVolumeImage& out = scattering_[static_cast<std::size_t>(current_)];
    VkVolumeImage& history = scattering_[static_cast<std::size_t>(1 - current_)];
    out.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    history.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    if (f.fluidCount > 0) {
        // The fluids stepped earlier in the frame: their writes before these reads.
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);
    }
    smokeLight_.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT);
    lightStage_.dispatch(renderer, cb,
                         {VkComputeResource::bufferRange(params_[slot].buffer, 0, sizeof(GpuParams)),
                          VkComputeResource::sampled(f.shadowAtlas, f.shadowSampler),
                          VkComputeResource::bufferRange(f.clusterParams),
                          VkComputeResource::bufferRange(f.clusterLights),
                          VkComputeResource::bufferRange(f.clusterCells),
                          VkComputeResource::bufferRange(f.clusterItems),
                          VkComputeResource::bufferRange(volumes_[slot].buffer),
                          VkComputeResource::sampled(history.view(), linear_, VK_IMAGE_LAYOUT_GENERAL),
                          VkComputeResource::image(out.view()),
                          VkComputeResource::image(smokeLight_.view())},
                         nullptr, (kX + 7) / 8, (kY + 7) / 8, kZ);
    out.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    integrated_.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT);
    integrateStage_.dispatch(renderer, cb,
                             {VkComputeResource::bufferRange(params_[slot].buffer, 0, sizeof(GpuParams)),
                              VkComputeResource::image(out.view()), VkComputeResource::image(integrated_.view())},
                             nullptr, (kX + 7) / 8, (kY + 7) / 8, 1);
    integrated_.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    marchSmoke(renderer, cb, f, slot, history.view());
    current_ = 1 - current_;
    historyValid_ = true;
}

void VkFroxelFog::marchSmoke(VulkanRenderer& renderer, VkCommandBuffer cb, const Frame& f, std::size_t slot,
                             VkImageView spare) {
    if (!smoke_.view()) return;
    if (!smokeReady_) {
        for (VkVolumeImage* image : {&smoke_, &shimmer_}) {
            image->discard();
            image->transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_SHADER_READ_BIT);
        }
        smokeReady_ = true;
    }
    smokeThisFrame_ = f.fluidCount > 0;
    shimmerThisFrame_ = f.fluidCount > 0 || !f.heat.empty();
    if (!shimmerThisFrame_) return;
    smoke_.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    shimmer_.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    smokeLight_.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_READ_BIT);
    // An unused fluid slot still needs something bound.
    const auto fluidAt = [&](std::size_t i) {
        return i < f.fluidCount && f.fluids[i].density != VK_NULL_HANDLE ? f.fluids[i].density : spare;
    };
    const float push[8] = {f.depthA, f.depthB, static_cast<float>(extent_.width), static_cast<float>(extent_.height),
                           static_cast<float>(f.frameIndex % 64u), 0.0f, 0.0f, 0.0f};
    smokeStage_.dispatch(renderer, cb,
                         {VkComputeResource::bufferRange(params_[slot].buffer, 0, sizeof(GpuParams)),
                          VkComputeResource::sampled(sceneDepth_, nearest_,
                                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
                          VkComputeResource::sampled(integrated_.view(), linear_, VK_IMAGE_LAYOUT_GENERAL),
                          VkComputeResource::sampled(smokeLight_.view(), linear_, VK_IMAGE_LAYOUT_GENERAL),
                          VkComputeResource::sampled(fluidAt(0), linear_, VK_IMAGE_LAYOUT_GENERAL),
                          VkComputeResource::sampled(fluidAt(1), linear_, VK_IMAGE_LAYOUT_GENERAL),
                          VkComputeResource::sampled(fluidAt(2), linear_, VK_IMAGE_LAYOUT_GENERAL),
                          VkComputeResource::image(smoke_.view()), VkComputeResource::image(shimmer_.view()),
                          VkComputeResource::sampled(fluidAt(3), linear_, VK_IMAGE_LAYOUT_GENERAL)},
                         push, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
    smoke_.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    shimmer_.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

void VkFroxelFog::apply(VkCommandBuffer cb, const Frame& f) {
    if (!applyFramebuffer_) return;
    VkRenderPassBeginInfo begin{};
    begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass        = applyPass_;
    begin.framebuffer       = applyFramebuffer_;
    begin.renderArea.extent = extent_;
    vkCmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height),
                              0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, applyPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, applyLayout_, 0, 1, &applySet_, 0, nullptr);
    const float far = std::max(f.fog.distance, kNear * 4.0f);
    const float push[8] = {f.depthA, f.depthB, kNear, std::log(far / kNear), static_cast<float>(kZ), far,
                           smokeThisFrame_ ? 1.0f : 0.0f, 0.0f};
    vkCmdPushConstants(cb, applyLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
}

void VkFroxelFog::destroy(VkContext& ctx) {
    const VkDevice device = ctx.device();
    destroyTargets(ctx);
    lightStage_.destroy(ctx);
    integrateStage_.destroy(ctx);
    smokeStage_.destroy(ctx);
    smokeLight_.destroy(ctx);
    for (auto& s : scattering_) s.destroy(ctx);
    integrated_.destroy(ctx);
    for (std::size_t i = 0; i < params_.size(); ++i) {
        for (Buffer* b : {&params_[i], &volumes_[i]}) {
            if (b->buffer) vmaDestroyBuffer(ctx.allocator(), b->buffer, b->alloc);
            *b = Buffer{};
        }
    }
    if (linear_) vkDestroySampler(device, linear_, nullptr);
    if (nearest_) vkDestroySampler(device, nearest_, nullptr);
    if (applyPipeline_) vkDestroyPipeline(device, applyPipeline_, nullptr);
    if (applyLayout_) vkDestroyPipelineLayout(device, applyLayout_, nullptr);
    if (applyPool_) vkDestroyDescriptorPool(device, applyPool_, nullptr);
    if (applySetLayout_) vkDestroyDescriptorSetLayout(device, applySetLayout_, nullptr);
    if (applyPass_) vkDestroyRenderPass(device, applyPass_, nullptr);
    linear_ = nearest_ = VK_NULL_HANDLE;
    applyPipeline_ = VK_NULL_HANDLE;
    applyLayout_ = VK_NULL_HANDLE;
    applyPool_ = VK_NULL_HANDLE;
    applySetLayout_ = VK_NULL_HANDLE;
    applyPass_ = VK_NULL_HANDLE;
}

}  // namespace iron
