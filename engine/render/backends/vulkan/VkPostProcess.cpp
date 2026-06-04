// VkPostProcess.cpp — offscreen scene-color target + full-screen copy pipeline.

#include "render/backends/vulkan/VkPostProcess.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "render/Ssao.h"  // M48: generateSsaoKernel
#include "scene/Mesh.h"
#include "core/Log.h"

#include <algorithm>  // std::min / std::max (bloom mip math)
#include <cstring>    // std::memcpy (SSAO noise upload)

namespace iron {

namespace {

const char* kFullscreenVert = R"(#version 450
layout(location = 0) out vec2 vUV;
void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kCopyFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(push_constant) uniform Push { float exposure; } pc;
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main() {
    vec3 hdr = texture(uScene, vUV).rgb * pc.exposure;
    outColor = vec4(aces(hdr), 1.0);
}
)";

// Composite (tonemap) fragment shader — a bloom-aware variant of kCopyFrag used
// ONLY by the copy/composite pipeline (NOT the viewport->swapchain blit, which
// keeps the binding-0-only kCopyFrag). Adds bloom mip0 (binding 1) before the
// ACES curve. The aces() body and the exposure/output are byte-identical to
// kCopyFrag; the only additions are the uBloom binding, the bloomIntensity push
// field, and the `scene + bloom * intensity` add. (M47) M48 adds the uSsao
// binding (2), the aoStrength push field, and the `scene * ao` multiply.
const char* kCompositeFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform sampler2D uBloom;
layout(set = 0, binding = 2) uniform sampler2D uSsao;   // M48 — blurred AO
layout(push_constant) uniform Push { float exposure; float bloomIntensity; float aoStrength; } pc;
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    vec3 bloom = texture(uBloom, vUV).rgb;
    float ao   = mix(1.0, texture(uSsao, vUV).r, pc.aoStrength);   // M48
    vec3 hdr   = (scene * ao + bloom * pc.bloomIntensity) * pc.exposure;  // AO darkens scene, not bloom
    outColor = vec4(aces(hdr), 1.0);
}
)";

// --- Mask pass shaders ---
// Position-only; MVP from push constant. The vertex buffer is the full Vertex
// struct (stride = sizeof(Vertex)) with position at location 0, mirroring
// VkShadowMap's vertex input setup so normal mesh vertex buffers bind directly.
const char* kMaskVert = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(push_constant) uniform Push { mat4 mvp; uint id; } pc;
void main() { gl_Position = pc.mvp * vec4(aPos, 1.0); }
)";

// Writes the effect id into the R8_UINT color attachment.
const char* kMaskFrag = R"(#version 450
layout(push_constant) uniform Push { mat4 mvp; uint id; } pc;
layout(location = 0) out uint outId;
void main() { outId = pc.id; }
)";

// --- Outline pass shaders ---
// Full-screen triangle (vertex = kFullscreenVert). Overlay: samples only the R8_UINT
// mask id (binding 0), edge-detects it with a 3x3 kernel, and outputs the outline color
// with alpha=edge so it alpha-blends over the Copy composite (which carries the scene +
// bloom + SSAO + tonemap).
const char* kOutlineFrag = R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform usampler2D uMask;   // overlay: scene comes from the Copy base
layout(push_constant) uniform Push { vec4 color; vec2 texel; float width; } pc;

void main() {
    uint here = texture(uMask, vUV).r;
    float edge = 0.0;
    for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy) {
        if (dx == 0 && dy == 0) continue;
        vec2 o = vec2(float(dx), float(dy)) * pc.texel * pc.width;
        uint n = texture(uMask, vUV + o).r;
        if (n != here) edge = 1.0;
    }
    outColor = vec4(pc.color.rgb, edge);   // alpha-blend over the composited scene
}
)";

// --- Glow blur + composite shaders ---

// kGlowBlurHFrag — reads the R8_UINT mask, converts to coverage (0 or 1),
// then horizontally blurs it using a 13-tap Gaussian-ish kernel. Sampling
// the mask directly in the blur pass folds the coverage→float step in,
// preserving the 3-pass plan (GlowBlurH, GlowBlurV, GlowComposite).
const char* kGlowBlurHFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out float outCov;
layout(set = 0, binding = 0) uniform usampler2D uMask;
layout(push_constant) uniform Push { vec2 texel; float radius; float _pad; } pc;
void main() {
    float sum = 0.0, wsum = 0.0;
    for (int i = -6; i <= 6; ++i) {
        float w = exp(-float(i * i) / 18.0);
        vec2 o = vec2(float(i) * pc.radius / 6.0, 0.0) * pc.texel;
        float cov = texture(uMask, vUV + o).r > 0u ? 1.0 : 0.0;
        sum += cov * w; wsum += w;
    }
    outCov = sum / wsum;
}
)";

// kGlowBlurVFrag — reads blurred coverage from scratch[0] (sampler2D) and
// blurs it vertically into scratch[1].
const char* kGlowBlurVFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out float outCov;
layout(set = 0, binding = 0) uniform sampler2D uCov;
layout(push_constant) uniform Push { vec2 texel; float radius; float _pad; } pc;
void main() {
    float sum = 0.0, wsum = 0.0;
    for (int i = -6; i <= 6; ++i) {
        float w = exp(-float(i * i) / 18.0);
        vec2 o = vec2(0.0, float(i) * pc.radius / 6.0) * pc.texel;
        sum += texture(uCov, vUV + o).r * w; wsum += w;
    }
    outCov = sum / wsum;
}
)";

// kGlowCompositeFrag — overlay: emits a colored halo (blurred coverage minus
// solid interior) and ADDITIVELY blends (ONE/ONE) over the Copy composite (which
// carries the scene + bloom + SSAO + tonemap). Samples no scene color and does no
// tonemap.
// binding 0: sampler2D  uBlur — blurred coverage (GlowBlurV output, scratch[1])
// binding 1: usampler2D uMask — tagged object id (0 = no object)
const char* kGlowCompositeFrag = R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D  uBlur;
layout(set = 0, binding = 1) uniform usampler2D uMask;
layout(push_constant) uniform Push { vec4 color; float intensity; } pc;

void main() {
    float blur  = texture(uBlur, vUV).r;
    float solid = texture(uMask, vUV).r > 0u ? 1.0 : 0.0;
    float halo  = max(blur - solid, 0.0) * pc.intensity;
    outColor = vec4(pc.color.rgb * halo, 1.0);   // additive blend (ONE/ONE) over the scene
}
)";

// kXrayFrag — overlay: tints where the tagged object is occluded by nearer scene
// geometry, outputting only the tint with alpha so it alpha-blends over the Copy
// composite (which carries the scene + bloom + SSAO + tonemap). Samples no scene
// color and does no tonemap.
// binding 0: usampler2D uMask       — tagged object id (0 = no object)
// binding 1: sampler2D  uMaskDepth  — tagged object's own depth (from mask pass)
// binding 2: sampler2D  uSceneDepth — full scene depth (includes occluders)
// Emits alpha = occluded * intensity where id != 0 AND sceneDepth < maskDepth.
// Both depths use the same projection, so raw float comparison is correct;
// smaller depth value means nearer to the camera.
const char* kXrayFrag = R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform usampler2D uMask;
layout(set = 0, binding = 1) uniform sampler2D  uMaskDepth;
layout(set = 0, binding = 2) uniform sampler2D  uSceneDepth;
layout(push_constant) uniform Push { vec4 color; float intensity; } pc;

void main() {
    uint id = texture(uMask, vUV).r;
    if (id == 0u) { outColor = vec4(0.0); return; }     // not tagged: transparent
    float md = texture(uMaskDepth, vUV).r;
    float sd = texture(uSceneDepth, vUV).r;
    float occluded = (sd < md - 1e-4) ? 1.0 : 0.0;      // nearer geometry in front
    outColor = vec4(pc.color.rgb, occluded * pc.intensity);   // alpha-blend tint
}
)";

}  // namespace

// ---------------------------------------------------------------------------
// Bloom fragment shaders (M47). Public (declared in the header) so test_bloom
// can compile-check them to SPIR-V. Reuse the anonymous-namespace kFullscreenVert
// for the vertex stage when building the actual pipelines.
// ---------------------------------------------------------------------------

const char* kBloomPrefilterDownSrc() {
    return R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(push_constant) uniform PC { float threshold; float knee; vec2 srcTexel; } pc;

vec3 prefilter(vec3 c) {
    float br = max(c.r, max(c.g, c.b));
    float soft = br - pc.threshold + pc.knee;
    soft = clamp(soft, 0.0, 2.0 * pc.knee);
    soft = soft * soft / (4.0 * pc.knee + 1e-4);
    float contrib = max(soft, br - pc.threshold);
    contrib /= max(br, 1e-4);
    return c * contrib;
}
float karis(vec3 c) { float l = dot(c, vec3(0.2126, 0.7152, 0.0722)); return 1.0 / (1.0 + l); }

void main() {
    vec2 t = pc.srcTexel; vec2 uv = vUV;
    vec3 a = texture(uSrc, uv + t * vec2(-2.0,  2.0)).rgb;
    vec3 b = texture(uSrc, uv + t * vec2( 0.0,  2.0)).rgb;
    vec3 c = texture(uSrc, uv + t * vec2( 2.0,  2.0)).rgb;
    vec3 d = texture(uSrc, uv + t * vec2(-2.0,  0.0)).rgb;
    vec3 e = texture(uSrc, uv + t * vec2( 0.0,  0.0)).rgb;
    vec3 f = texture(uSrc, uv + t * vec2( 2.0,  0.0)).rgb;
    vec3 g = texture(uSrc, uv + t * vec2(-2.0, -2.0)).rgb;
    vec3 h = texture(uSrc, uv + t * vec2( 0.0, -2.0)).rgb;
    vec3 i = texture(uSrc, uv + t * vec2( 2.0, -2.0)).rgb;
    vec3 j = texture(uSrc, uv + t * vec2(-1.0,  1.0)).rgb;
    vec3 k = texture(uSrc, uv + t * vec2( 1.0,  1.0)).rgb;
    vec3 l = texture(uSrc, uv + t * vec2(-1.0, -1.0)).rgb;
    vec3 m = texture(uSrc, uv + t * vec2( 1.0, -1.0)).rgb;
    vec3 box0 = (j + k + l + m) * 0.25;
    vec3 box1 = (a + b + d + e) * 0.25;
    vec3 box2 = (b + c + e + f) * 0.25;
    vec3 box3 = (d + e + g + h) * 0.25;
    vec3 box4 = (e + f + h + i) * 0.25;
    box0 = prefilter(box0); box1 = prefilter(box1); box2 = prefilter(box2);
    box3 = prefilter(box3); box4 = prefilter(box4);
    float w0 = karis(box0) * 0.5;
    float w1 = karis(box1) * 0.125;
    float w2 = karis(box2) * 0.125;
    float w3 = karis(box3) * 0.125;
    float w4 = karis(box4) * 0.125;
    vec3 sum = box0 * w0 + box1 * w1 + box2 * w2 + box3 * w3 + box4 * w4;
    float wsum = w0 + w1 + w2 + w3 + w4;
    outColor = vec4(sum / max(wsum, 1e-4), 1.0);
}
)";
}

const char* kBloomDownsampleSrc() {
    return R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(push_constant) uniform PC { vec2 srcTexel; } pc;

void main() {
    vec2 t = pc.srcTexel; vec2 uv = vUV;
    vec3 a = texture(uSrc, uv + t * vec2(-2.0,  2.0)).rgb;
    vec3 b = texture(uSrc, uv + t * vec2( 0.0,  2.0)).rgb;
    vec3 c = texture(uSrc, uv + t * vec2( 2.0,  2.0)).rgb;
    vec3 d = texture(uSrc, uv + t * vec2(-2.0,  0.0)).rgb;
    vec3 e = texture(uSrc, uv + t * vec2( 0.0,  0.0)).rgb;
    vec3 f = texture(uSrc, uv + t * vec2( 2.0,  0.0)).rgb;
    vec3 g = texture(uSrc, uv + t * vec2(-2.0, -2.0)).rgb;
    vec3 h = texture(uSrc, uv + t * vec2( 0.0, -2.0)).rgb;
    vec3 i = texture(uSrc, uv + t * vec2( 2.0, -2.0)).rgb;
    vec3 j = texture(uSrc, uv + t * vec2(-1.0,  1.0)).rgb;
    vec3 k = texture(uSrc, uv + t * vec2( 1.0,  1.0)).rgb;
    vec3 l = texture(uSrc, uv + t * vec2(-1.0, -1.0)).rgb;
    vec3 m = texture(uSrc, uv + t * vec2( 1.0, -1.0)).rgb;
    vec3 r  = (j + k + l + m) * 0.5   * 0.25;
    r      += (a + b + d + e) * 0.125 * 0.25;
    r      += (b + c + e + f) * 0.125 * 0.25;
    r      += (d + e + g + h) * 0.125 * 0.25;
    r      += (e + f + h + i) * 0.125 * 0.25;
    outColor = vec4(r, 1.0);
}
)";
}

const char* kBloomUpsampleSrc() {
    return R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(push_constant) uniform PC { vec2 srcTexel; float scatter; } pc;

void main() {
    vec2 t = pc.srcTexel * pc.scatter; vec2 uv = vUV;
    vec3 s = vec3(0.0);
    s += texture(uSrc, uv + t * vec2(-1.0,  1.0)).rgb * 1.0;
    s += texture(uSrc, uv + t * vec2( 0.0,  1.0)).rgb * 2.0;
    s += texture(uSrc, uv + t * vec2( 1.0,  1.0)).rgb * 1.0;
    s += texture(uSrc, uv + t * vec2(-1.0,  0.0)).rgb * 2.0;
    s += texture(uSrc, uv + t * vec2( 0.0,  0.0)).rgb * 4.0;
    s += texture(uSrc, uv + t * vec2( 1.0,  0.0)).rgb * 2.0;
    s += texture(uSrc, uv + t * vec2(-1.0, -1.0)).rgb * 1.0;
    s += texture(uSrc, uv + t * vec2( 0.0, -1.0)).rgb * 2.0;
    s += texture(uSrc, uv + t * vec2( 1.0, -1.0)).rgb * 1.0;
    outColor = vec4(s * (1.0 / 16.0), 1.0);
}
)";
}

// ---------------------------------------------------------------------------
// SSAO fragment shaders (M48). Public (declared in the header) so test_ssao
// can compile-check them to SPIR-V. Reuse the anonymous-namespace
// kFullscreenVert for the vertex stage when building the actual pipelines.
// ---------------------------------------------------------------------------

const char* kSsaoSrc() {
    return R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;   // R8: write .r
layout(binding = 0) uniform sampler2D uDepth;
layout(binding = 1) uniform sampler2D uNoise;
layout(binding = 2, std140) uniform SsaoUbo {
    mat4 projection;
    mat4 invProjection;
    vec4 kernel[32];     // .xyz = view-space hemisphere sample
    vec4 params;         // x=radius, y=bias, z=power, w=sampleCount
    vec4 noiseScale;     // xy = extent / 4
} u;

vec3 reconstructViewPos(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);  // Vulkan clip z is [0,1]
    vec4 v = u.invProjection * clip;
    return v.xyz / v.w;
}

void main() {
    float depth = texture(uDepth, vUV).r;
    if (depth >= 1.0) { outColor = vec4(1.0); return; }   // background: unoccluded
    vec3 P = reconstructViewPos(vUV, depth);
    // Robust view-space normal from depth: pick the nearer horizontal/vertical
    // neighbor on each axis (Drobot) to avoid the spikes at depth discontinuities
    // that naive cross(dFdx,dFdy) produces (dark silhouette halos). Then force the
    // normal to face the camera (origin in view space, looking down -z).
    vec2 texel = 1.0 / vec2(textureSize(uDepth, 0));
    vec3 Pr = reconstructViewPos(vUV + vec2(texel.x, 0.0), texture(uDepth, vUV + vec2(texel.x, 0.0)).r);
    vec3 Pl = reconstructViewPos(vUV - vec2(texel.x, 0.0), texture(uDepth, vUV - vec2(texel.x, 0.0)).r);
    vec3 Pu = reconstructViewPos(vUV + vec2(0.0, texel.y), texture(uDepth, vUV + vec2(0.0, texel.y)).r);
    vec3 Pd = reconstructViewPos(vUV - vec2(0.0, texel.y), texture(uDepth, vUV - vec2(0.0, texel.y)).r);
    vec3 ddxP = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
    vec3 ddyP = (abs(Pu.z - P.z) < abs(P.z - Pd.z)) ? (Pu - P) : (P - Pd);
    vec3 N = normalize(cross(ddxP, ddyP));
    if (dot(N, P) > 0.0) N = -N;   // face the camera
    vec3 rnd = texture(uNoise, vUV * u.noiseScale.xy).xyz;
    vec3 T = normalize(rnd - N * dot(rnd, N));   // Gram-Schmidt
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float radius = u.params.x;
    float bias   = u.params.y;
    int   count  = int(u.params.w);
    float occlusion = 0.0;
    for (int i = 0; i < count; ++i) {
        vec3 samplePos = P + TBN * u.kernel[i].xyz * radius;   // view space
        vec4 off = u.projection * vec4(samplePos, 1.0);
        off.xyz /= off.w;
        vec2 sampleUv = off.xy * 0.5 + 0.5;
        float sd = texture(uDepth, sampleUv).r;
        vec3 surface = reconstructViewPos(sampleUv, sd);
        float rangeCheck = smoothstep(0.0, 1.0, radius / max(abs(P.z - surface.z), 1e-4));
        occlusion += (surface.z >= samplePos.z + bias ? 1.0 : 0.0) * rangeCheck;
    }
    float ao = 1.0 - occlusion / float(count);
    ao = pow(clamp(ao, 0.0, 1.0), u.params.z);
    outColor = vec4(ao, ao, ao, 1.0);
}
)";
}

const char* kSsaoBlurSrc() {
    return R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uSsao;
layout(push_constant) uniform PC { vec2 texel; } pc;   // 1 / extent

void main() {
    float sum = 0.0;
    for (int x = -2; x < 2; ++x)
        for (int y = -2; y < 2; ++y)
            sum += texture(uSsao, vUV + vec2(float(x), float(y)) * pc.texel).r;
    float ao = sum / 16.0;
    outColor = vec4(ao, ao, ao, 1.0);
}
)";
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool VkPostProcess::init(VkContext& ctx, VkFormat colorFormat, VkFormat depthFormat,
                         VkExtent2D extent, VkSampler sharedSampler,
                         VkRenderPass swapchainPass) {
    ctx_             = &ctx;
    colorFormat_     = colorFormat;
    depthFormat_     = depthFormat;
    extent_          = extent;
    viewportExtent_  = extent;
    sampler_         = sharedSampler;

    // Create a NEAREST sampler for integer textures (R8_UINT mask).
    // The shared sampler_ from VkTextureStore is LINEAR; integer formats
    // cannot be linearly filtered (Vulkan validation error), so a dedicated
    // NEAREST sampler is required here.
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_NEAREST;
        si.minFilter    = VK_FILTER_NEAREST;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(ctx.device(), &si, nullptr, &maskSampler_));
    }

    // M47: dedicated bloom sampler — LINEAR filter with CLAMP_TO_EDGE addressing.
    // The bloom down/upsample shaders take offset filter taps; with the shared
    // sampler_'s REPEAT addressing those taps wrap around screen/mip edges and
    // bleed light across edges. Each bloom pass samples a single mip via a
    // distinct image view, so no mip LOD is needed (mipmapMode NEAREST, maxLod 0).
    // Must be created BEFORE createTargets(ctx) below, since createBloomTargets
    // (invoked from createTargets) writes the bloom descriptor sets with it.
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.minLod       = 0.0f;
        si.maxLod       = 0.0f;
        VK_CHECK(vkCreateSampler(ctx.device(), &si, nullptr, &bloomSampler_));
    }

    // M48 SSAO persistent setup (noise texture, UBO, kernel) + the set layouts and
    // descriptor pool. These MUST exist BEFORE createTargets(ctx) below, since
    // createSsaoTargets (invoked from createTargets) allocates + writes the SSAO
    // descriptor sets and references the noise view/sampler + UBO. Heeds the M47
    // null-layout lesson: layouts + pool created before the sets are allocated.
    if (!createSsaoNoiseAndUbo(ctx))               { destroy(ctx); return false; }
    {
        // ssaoSetLayout_: {0 depth, 1 noise, 2 ubo}; ssaoBlurSetLayout_: {0 ssaoTex}.
        VkDescriptorSetLayoutBinding sb[3]{};
        sb[0].binding = 0; sb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sb[0].descriptorCount = 1; sb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        sb[1].binding = 1; sb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sb[1].descriptorCount = 1; sb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        sb[2].binding = 2; sb[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sb[2].descriptorCount = 1; sb[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 3; li.pBindings = sb;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &li, nullptr, &ssaoSetLayout_));

        VkDescriptorSetLayoutBinding bb{};
        bb.binding = 0; bb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bb.descriptorCount = 1; bb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo bli{};
        bli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        bli.bindingCount = 1; bli.pBindings = &bb;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &bli, nullptr, &ssaoBlurSetLayout_));

        // Dedicated persistent pool: 3 sets (2 per-frame ssao sets + 1 blur set);
        // 5 combined-image-samplers (2 ssao sets * (depth+noise)=4, blurSet:
        // ssaoTex=1) + 2 uniform buffers (1 per ssao set).
        VkDescriptorPoolSize ps[2]{};
        ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[0].descriptorCount = 5;
        ps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         ps[1].descriptorCount = 2;
        VkDescriptorPoolCreateInfo dp{};
        dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.maxSets = 3; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
        VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dp, nullptr, &ssaoDescPool_));
    }

    if (!createTargets(ctx))                       { destroy(ctx); return false; }
    if (!createViewportTarget(ctx))                { destroy(ctx); return false; }
    if (!createCopyPipeline(ctx))                  { destroy(ctx); return false; }
    if (!createBlitPipeline(ctx, swapchainPass))   { destroy(ctx); return false; }
    if (!createOutlinePipeline(ctx))               { destroy(ctx); return false; }
    if (!createMaskPipeline(ctx))                  { destroy(ctx); return false; }
    if (!createGlowPipelines(ctx))                 { destroy(ctx); return false; }
    if (!createXRayPipeline(ctx))                  { destroy(ctx); return false; }

    // M47 bloom: the mip-chain target (image + per-mip views/fbs + pass) is created
    // by createTargets(ctx) above and rebuilt on resize in createTargets()/destroyTargets();
    // the pipelines/layouts/set-layout are persistent (built once here, torn down in destroy()).
    if (!createBloomPipelines())                   { destroy(ctx); return false; }

    // M48 SSAO pipelines: built against ssaoPass_, which createTargets(ctx) above
    // created (via createSsaoTargets). Persistent (torn down in destroy()).
    if (!createSsaoPipelines(ctx))                 { destroy(ctx); return false; }
    return true;
}

void VkPostProcess::destroy(VkContext& ctx) {
    // M48 SSAO persistent objects (pipelines, layouts, set layouts, pool, noise, ubo).
    // Per-resize SSAO handles (images/views/fbs/sets) are freed by destroyTargets below.
    if (ssaoBlurPipeline_) { vkDestroyPipeline(ctx.device(), ssaoBlurPipeline_, nullptr); ssaoBlurPipeline_ = VK_NULL_HANDLE; }
    if (ssaoPipeline_)     { vkDestroyPipeline(ctx.device(), ssaoPipeline_, nullptr);     ssaoPipeline_ = VK_NULL_HANDLE; }
    if (ssaoBlurLayout_)   { vkDestroyPipelineLayout(ctx.device(), ssaoBlurLayout_, nullptr); ssaoBlurLayout_ = VK_NULL_HANDLE; }
    if (ssaoLayout_)       { vkDestroyPipelineLayout(ctx.device(), ssaoLayout_, nullptr);     ssaoLayout_ = VK_NULL_HANDLE; }
    if (ssaoBlurSetLayout_){ vkDestroyDescriptorSetLayout(ctx.device(), ssaoBlurSetLayout_, nullptr); ssaoBlurSetLayout_ = VK_NULL_HANDLE; }
    if (ssaoSetLayout_)    { vkDestroyDescriptorSetLayout(ctx.device(), ssaoSetLayout_, nullptr);     ssaoSetLayout_ = VK_NULL_HANDLE; }
    // The pool owns ssaoSet_[0/1]/ssaoBlurSet_; destroying it frees them.
    if (ssaoDescPool_)     { vkDestroyDescriptorPool(ctx.device(), ssaoDescPool_, nullptr); ssaoDescPool_ = VK_NULL_HANDLE; ssaoSet_[0] = VK_NULL_HANDLE; ssaoSet_[1] = VK_NULL_HANDLE; ssaoBlurSet_ = VK_NULL_HANDLE; }
    if (ssaoNoiseSampler_) { vkDestroySampler(ctx.device(), ssaoNoiseSampler_, nullptr); ssaoNoiseSampler_ = VK_NULL_HANDLE; }
    if (ssaoNoiseView_)    { vkDestroyImageView(ctx.device(), ssaoNoiseView_, nullptr); ssaoNoiseView_ = VK_NULL_HANDLE; }
    if (ssaoNoise_)        { vmaDestroyImage(ctx.allocator(), ssaoNoise_, ssaoNoiseAlloc_); ssaoNoise_ = VK_NULL_HANDLE; ssaoNoiseAlloc_ = VK_NULL_HANDLE; }
    for (int f = 0; f < 2; ++f) {
        if (ssaoUboBuf_[f]) { vmaDestroyBuffer(ctx.allocator(), ssaoUboBuf_[f], ssaoUboAlloc_[f]); ssaoUboBuf_[f] = VK_NULL_HANDLE; ssaoUboAlloc_[f] = VK_NULL_HANDLE; }
        ssaoUboMapped_[f] = nullptr;
    }

    // M47 bloom pipelines/layouts/set-layout (persistent — not per-resize).
    if (bloomUpPipeline_)         { vkDestroyPipeline(ctx.device(), bloomUpPipeline_, nullptr);         bloomUpPipeline_ = VK_NULL_HANDLE; }
    if (bloomDownPipeline_)       { vkDestroyPipeline(ctx.device(), bloomDownPipeline_, nullptr);       bloomDownPipeline_ = VK_NULL_HANDLE; }
    if (bloomBrightDownPipeline_) { vkDestroyPipeline(ctx.device(), bloomBrightDownPipeline_, nullptr); bloomBrightDownPipeline_ = VK_NULL_HANDLE; }
    if (bloomUpLayout_)           { vkDestroyPipelineLayout(ctx.device(), bloomUpLayout_, nullptr);           bloomUpLayout_ = VK_NULL_HANDLE; }
    if (bloomDownLayout_)         { vkDestroyPipelineLayout(ctx.device(), bloomDownLayout_, nullptr);         bloomDownLayout_ = VK_NULL_HANDLE; }
    if (bloomBrightDownLayout_)   { vkDestroyPipelineLayout(ctx.device(), bloomBrightDownLayout_, nullptr);   bloomBrightDownLayout_ = VK_NULL_HANDLE; }
    if (bloomSetLayout_)          { vkDestroyDescriptorSetLayout(ctx.device(), bloomSetLayout_, nullptr);     bloomSetLayout_ = VK_NULL_HANDLE; }

    // X-ray pipeline objects.
    if (xrayPipeline_)   { vkDestroyPipeline(ctx.device(), xrayPipeline_, nullptr); xrayPipeline_ = VK_NULL_HANDLE; }
    if (xrayPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), xrayPipeLayout_, nullptr); xrayPipeLayout_ = VK_NULL_HANDLE; }
    if (xraySetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), xraySetLayout_, nullptr); xraySetLayout_ = VK_NULL_HANDLE; }
    xrayDescSet_ = VK_NULL_HANDLE;  // freed with descPool_ below

    // Mask pipeline objects (no descriptor set layout — push-constant only).
    if (maskPipeline_)   { vkDestroyPipeline(ctx.device(), maskPipeline_, nullptr); maskPipeline_ = VK_NULL_HANDLE; }
    if (maskPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), maskPipeLayout_, nullptr); maskPipeLayout_ = VK_NULL_HANDLE; }

    // Outline pipeline objects.
    if (outlinePipeline_)   { vkDestroyPipeline(ctx.device(), outlinePipeline_, nullptr); outlinePipeline_ = VK_NULL_HANDLE; }
    if (outlinePipeLayout_) { vkDestroyPipelineLayout(ctx.device(), outlinePipeLayout_, nullptr); outlinePipeLayout_ = VK_NULL_HANDLE; }
    if (outlineSetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), outlineSetLayout_, nullptr); outlineSetLayout_ = VK_NULL_HANDLE; }
    outlineDescSet_ = VK_NULL_HANDLE;  // freed with descPool_ below

    // Glow pipeline objects.
    if (glowCompositePipeline_)   { vkDestroyPipeline(ctx.device(), glowCompositePipeline_, nullptr);   glowCompositePipeline_ = VK_NULL_HANDLE; }
    if (glowCompositePipeLayout_) { vkDestroyPipelineLayout(ctx.device(), glowCompositePipeLayout_, nullptr); glowCompositePipeLayout_ = VK_NULL_HANDLE; }
    if (glowCompositeSetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), glowCompositeSetLayout_, nullptr); glowCompositeSetLayout_ = VK_NULL_HANDLE; }
    glowCompositeDescSet_ = VK_NULL_HANDLE;

    if (glowBlurVPipeline_)   { vkDestroyPipeline(ctx.device(), glowBlurVPipeline_, nullptr);   glowBlurVPipeline_ = VK_NULL_HANDLE; }
    if (glowBlurVPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), glowBlurVPipeLayout_, nullptr); glowBlurVPipeLayout_ = VK_NULL_HANDLE; }
    if (glowBlurVSetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), glowBlurVSetLayout_, nullptr); glowBlurVSetLayout_ = VK_NULL_HANDLE; }
    glowBlurVDescSet_ = VK_NULL_HANDLE;

    if (glowBlurHPipeline_)   { vkDestroyPipeline(ctx.device(), glowBlurHPipeline_, nullptr);   glowBlurHPipeline_ = VK_NULL_HANDLE; }
    if (glowBlurHPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), glowBlurHPipeLayout_, nullptr); glowBlurHPipeLayout_ = VK_NULL_HANDLE; }
    if (glowBlurHSetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), glowBlurHSetLayout_, nullptr); glowBlurHSetLayout_ = VK_NULL_HANDLE; }
    glowBlurHDescSet_ = VK_NULL_HANDLE;

    // Blit (viewport→swapchain) pipeline objects.
    if (blitPipeline_)   { vkDestroyPipeline(ctx.device(), blitPipeline_, nullptr); blitPipeline_ = VK_NULL_HANDLE; }
    if (blitPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), blitPipeLayout_, nullptr); blitPipeLayout_ = VK_NULL_HANDLE; }
    if (blitSetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), blitSetLayout_, nullptr); blitSetLayout_ = VK_NULL_HANDLE; }
    blitDescSet_ = VK_NULL_HANDLE;  // freed with descPool_ below

    // Copy (composite) pipeline objects.
    if (copyPipeline_)   { vkDestroyPipeline(ctx.device(), copyPipeline_, nullptr); copyPipeline_ = VK_NULL_HANDLE; }
    if (copyPipeLayout_) { vkDestroyPipelineLayout(ctx.device(), copyPipeLayout_, nullptr); copyPipeLayout_ = VK_NULL_HANDLE; }
    if (copySetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), copySetLayout_, nullptr); copySetLayout_ = VK_NULL_HANDLE; }
    // descPool_ owns all descriptor sets; destroying the pool frees them all.
    if (descPool_)       { vkDestroyDescriptorPool(ctx.device(), descPool_, nullptr); descPool_ = VK_NULL_HANDLE; copyDescSet_ = VK_NULL_HANDLE; }

    // NEAREST sampler for integer mask texture.
    if (maskSampler_)    { vkDestroySampler(ctx.device(), maskSampler_, nullptr); maskSampler_ = VK_NULL_HANDLE; }
    // M47: LINEAR + CLAMP_TO_EDGE sampler for the bloom mip chain.
    if (bloomSampler_)   { vkDestroySampler(ctx.device(), bloomSampler_, nullptr); bloomSampler_ = VK_NULL_HANDLE; }

    // Render target objects (images + framebuffers). The render passes are
    // persistent across resize, so they are NOT freed by these helpers —
    // destroy them here, at full teardown.
    destroyViewportTarget(ctx);
    destroyTargets(ctx);
    if (viewportPass_) { vkDestroyRenderPass(ctx.device(), viewportPass_, nullptr); viewportPass_ = VK_NULL_HANDLE; }
    if (bloomPass_)    { vkDestroyRenderPass(ctx.device(), bloomPass_, nullptr);    bloomPass_ = VK_NULL_HANDLE; }
    if (ssaoPass_)     { vkDestroyRenderPass(ctx.device(), ssaoPass_, nullptr);     ssaoPass_ = VK_NULL_HANDLE; }
    if (glowPass_)     { vkDestroyRenderPass(ctx.device(), glowPass_, nullptr);     glowPass_ = VK_NULL_HANDLE; }
    if (maskPass_)     { vkDestroyRenderPass(ctx.device(), maskPass_, nullptr);     maskPass_ = VK_NULL_HANDLE; }
    if (scenePass_)    { vkDestroyRenderPass(ctx.device(), scenePass_, nullptr);    scenePass_ = VK_NULL_HANDLE; }
}

bool VkPostProcess::resize(VkContext& ctx, VkExtent2D extent) {
    extent_ = extent;
    destroyTargets(ctx);
    if (!createTargets(ctx)) return false;

    // Re-write the copy/composite descriptor set: binding 0 = new sceneColorView_,
    // binding 1 = new bloom mip0, binding 2 = new blurred SSAO (createTargets above
    // rebuilt the bloom + SSAO targets, so bloomMipViews_[0] and ssaoBlurView_ are
    // the freshly-created views). ssaoBlurView_ uses bloomSampler_ (LINEAR+CLAMP). (M47/M48)
    {
        VkDescriptorImageInfo imgInfos[3]{};
        imgInfos[0].sampler     = sampler_;
        imgInfos[0].imageView   = sceneColorView_;
        imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[1].sampler     = bloomSampler_;
        imgInfos[1].imageView   = bloomMipViews_[0];
        imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[2].sampler     = bloomSampler_;
        imgInfos[2].imageView   = ssaoBlurView_;
        imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = copyDescSet_;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo      = &imgInfos[0];
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = copyDescSet_;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &imgInfos[1];
        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = copyDescSet_;
        writes[2].dstBinding      = 2;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].descriptorCount = 1;
        writes[2].pImageInfo      = &imgInfos[2];
        vkUpdateDescriptorSets(ctx.device(), 3, writes, 0, nullptr);
    }

    // Re-write the outline descriptor set to point at the new views.
    // Overlay: binding 0 = mask only (scene comes from the Copy base).
    {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = maskSampler_;
        imgInfo.imageView   = maskColorView_;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = outlineDescSet_;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }

    // Re-write glow descriptor sets to point at the new size-varying views.
    // GlowBlurH: binding 0 = maskColorView_ (NEAREST sampler).
    {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = maskSampler_;
        imgInfo.imageView   = maskColorView_;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = glowBlurHDescSet_;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }
    // GlowBlurV: binding 0 = glowScratchView_[0] (linear sampler).
    {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = sampler_;
        imgInfo.imageView   = glowScratchView_[0];
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = glowBlurVDescSet_;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }
    // GlowComposite (overlay — no scene sampler): binding 0 = glowScratchView_[1]
    //                                              (blur), binding 1 = maskColorView_.
    {
        VkDescriptorImageInfo imgInfos[2]{};
        imgInfos[0].sampler     = sampler_;
        imgInfos[0].imageView   = glowScratchView_[1];
        imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[1].sampler     = maskSampler_;
        imgInfos[1].imageView   = maskColorView_;
        imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        for (int i = 0; i < 2; ++i) {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = glowCompositeDescSet_;
            writes[i].dstBinding      = static_cast<uint32_t>(i);
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo      = &imgInfos[i];
        }
        vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);
    }

    // XRay (overlay — no scene sampler): binding 0 = maskColorView_,
    //        binding 1 = maskDepthView_, binding 2 = sceneDepthView_.
    {
        VkDescriptorImageInfo imgInfos[3]{};
        imgInfos[0].sampler     = maskSampler_;
        imgInfos[0].imageView   = maskColorView_;
        imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[1].sampler     = maskSampler_;
        imgInfos[1].imageView   = maskDepthView_;
        imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        imgInfos[2].sampler     = maskSampler_;
        imgInfos[2].imageView   = sceneDepthView_;
        imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[3]{};
        for (int i = 0; i < 3; ++i) {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = xrayDescSet_;
            writes[i].dstBinding      = static_cast<uint32_t>(i);
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo      = &imgInfos[i];
        }
        vkUpdateDescriptorSets(ctx.device(), 3, writes, 0, nullptr);
    }

    if (!resizeViewport(ctx, extent)) return false;

    return true;
}

void VkPostProcess::beginScenePass(VkCommandBuffer cb, const float clearColor[4]) const {
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = clearColor[0];
    clears[0].color.float32[1] = clearColor[1];
    clears[0].color.float32[2] = clearColor[2];
    clears[0].color.float32[3] = clearColor[3];
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = scenePass_;
    rpBegin.framebuffer       = sceneFb_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = extent_;
    rpBegin.clearValueCount   = 2;
    rpBegin.pClearValues      = clears;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x        = 0;
    vp.y        = 0;
    vp.width    = static_cast<float>(extent_.width);
    vp.height   = static_cast<float>(extent_.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissor(cb, 0, 1, &scissor);
}

void VkPostProcess::endScenePass(VkCommandBuffer cb) const {
    vkCmdEndRenderPass(cb);
}

void VkPostProcess::beginViewportPass(VkCommandBuffer cb, const float clearColor[4]) const {
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = clearColor[0];
    clears[0].color.float32[1] = clearColor[1];
    clears[0].color.float32[2] = clearColor[2];
    clears[0].color.float32[3] = clearColor[3];
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = viewportPass_;
    rpBegin.framebuffer       = viewportFb_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = viewportExtent_;
    rpBegin.clearValueCount   = 2;
    rpBegin.pClearValues      = clears;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Plain positive-height viewport covering the full viewport extent.
    // Composite draws are UV-space full-screen quads; debug/HUD passes
    // set their own viewport after this.
    VkViewport vp{};
    vp.x        = 0;
    vp.y        = 0;
    vp.width    = static_cast<float>(viewportExtent_.width);
    vp.height   = static_cast<float>(viewportExtent_.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, viewportExtent_};
    vkCmdSetScissor(cb, 0, 1, &scissor);
}

void VkPostProcess::endViewportPass(VkCommandBuffer cb) const {
    vkCmdEndRenderPass(cb);
}

void VkPostProcess::recordComposite(VkCommandBuffer cb, float exposure,
                                    float bloomIntensity, float aoStrength) const {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, copyPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            copyPipeLayout_, 0, 1, &copyDescSet_, 0, nullptr);
    CopyPush pc{};
    pc.exposure       = exposure;
    pc.bloomIntensity = bloomIntensity;
    pc.aoStrength     = aoStrength;
    vkCmdPushConstants(cb, copyPipeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(CopyPush), &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

void VkPostProcess::blitToSwapchain(VkCommandBuffer cb) const {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            blitPipeLayout_, 0, 1, &blitDescSet_, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

void VkPostProcess::beginMaskPass(VkCommandBuffer cb) const {
    // Clear color to 0 (no effect id), depth to 1.0 (far plane).
    VkClearValue clears[2]{};
    clears[0].color.uint32[0] = 0;
    clears[0].color.uint32[1] = 0;
    clears[0].color.uint32[2] = 0;
    clears[0].color.uint32[3] = 0;
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = maskPass_;
    rpBegin.framebuffer       = maskFb_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = extent_;
    rpBegin.clearValueCount   = 2;
    rpBegin.pClearValues      = clears;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Negative-height viewport so GL-style projection matrices render
    // right-side-up, consistent with the scene pass (VulkanRenderer::setSceneViewport).
    VkViewport vp{};
    vp.x        = 0;
    vp.y        = static_cast<float>(extent_.height);
    vp.width    = static_cast<float>(extent_.width);
    vp.height   = -static_cast<float>(extent_.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissor(cb, 0, 1, &scissor);
}

void VkPostProcess::endMaskPass(VkCommandBuffer cb) const {
    vkCmdEndRenderPass(cb);
}

void VkPostProcess::bindMaskPipeline(VkCommandBuffer cb) const {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, maskPipeline_);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool VkPostProcess::createTargets(VkContext& ctx) {
    // --- Scene color image (hdrFormat_, COLOR_ATTACHMENT | SAMPLED) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = hdrFormat_;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &sceneColor_, &sceneColorAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = sceneColor_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = hdrFormat_;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &sceneColorView_));
    }

    // --- Scene depth image (depthFormat_, DEPTH_STENCIL_ATTACHMENT | SAMPLED) ---
    // SAMPLED_BIT is required so the x-ray pass can compare scene depth against
    // the tagged object's own (mask) depth. The render pass transitions depth to
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL at the end so the x-ray shader can sample it.
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = depthFormat_;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &sceneDepth_, &sceneDepthAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = sceneDepth_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = depthFormat_;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &sceneDepthView_));
    }

    // --- Scene render pass: color + depth ---
    // Color final layout = SHADER_READ_ONLY_OPTIMAL so the composite pass
    // can sample it. Entry dep: FRAGMENT_SHADER -> COLOR_ATTACHMENT_OUTPUT
    // guards frame N+1 overwriting the image while frame N still samples it.
    // Exit dep: COLOR_ATTACHMENT_OUTPUT -> FRAGMENT_SHADER lets the composite
    // pass sample safely.
    VkAttachmentDescription attachments[2]{};
    attachments[0].format         = hdrFormat_;  // was colorFormat_ (scenePass_ color attachment)
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format         = depthFormat_;
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;  // must store so x-ray can sample it
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;  // sampleable for x-ray

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency deps[3]{};
    // Entry: wait for prior frame's sampling to finish before writing color + depth.
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    // Depth is written at LATE_FRAGMENT_TESTS; cover it so the
    // DEPTH_STENCIL_ATTACHMENT_WRITE access has a matching stage.
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // Exit (color): composite pass can sample color after writes complete.
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    // Exit (depth): x-ray pass can sample depth after depth writes complete.
    deps[2].srcSubpass    = 0;
    deps[2].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[2].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[2].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[2].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments    = attachments;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 3;
    rpInfo.pDependencies   = deps;
    // Render passes depend only on formats, not extent — create once and keep
    // across resize. Pipelines are built against this handle; recreating it on
    // resize would invalidate them (VUID-02684 / DEVICE_LOST). Destroyed in
    // destroy(), not destroyTargets().
    if (scenePass_ == VK_NULL_HANDLE)
        VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &scenePass_));

    // --- Scene framebuffer ---
    {
        VkImageView views[2] = {sceneColorView_, sceneDepthView_};
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = scenePass_;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments    = views;
        fbInfo.width           = extent_.width;
        fbInfo.height          = extent_.height;
        fbInfo.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &sceneFb_));
    }

    // -----------------------------------------------------------------------
    // Mask target: R8_UINT color + D32_SFLOAT depth.
    // Both have SAMPLED usage so later passes (outline, x-ray) can sample them.
    // -----------------------------------------------------------------------

    // --- Mask color image (R8_UINT, COLOR_ATTACHMENT | SAMPLED) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = VK_FORMAT_R8_UINT;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &maskColor_, &maskColorAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = maskColor_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = VK_FORMAT_R8_UINT;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &maskColorView_));
    }

    // --- Mask depth image (D32_SFLOAT, DEPTH_STENCIL_ATTACHMENT | SAMPLED) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = VK_FORMAT_D32_SFLOAT;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &maskDepth_, &maskDepthAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = maskDepth_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = VK_FORMAT_D32_SFLOAT;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &maskDepthView_));
    }

    // --- Mask render pass: R8_UINT color + D32 depth ---
    // Color finalLayout = SHADER_READ_ONLY_OPTIMAL so outline/glow can sample.
    // Depth finalLayout = DEPTH_STENCIL_READ_ONLY_OPTIMAL so x-ray can sample.
    // Subpass dependencies mirror VkReflectionTarget / scenePass_.
    {
        VkAttachmentDescription maskAttachments[2]{};
        // Color attachment (R8_UINT effectId).
        maskAttachments[0].format         = VK_FORMAT_R8_UINT;
        maskAttachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        maskAttachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        maskAttachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        maskAttachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        maskAttachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        maskAttachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        maskAttachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // Depth attachment (D32_SFLOAT, own depth buffer for tagged objects).
        maskAttachments[1].format         = VK_FORMAT_D32_SFLOAT;
        maskAttachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        maskAttachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        maskAttachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        maskAttachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        maskAttachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        maskAttachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        maskAttachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference maskColorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference maskDepthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription maskSubpass{};
        maskSubpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        maskSubpass.colorAttachmentCount    = 1;
        maskSubpass.pColorAttachments       = &maskColorRef;
        maskSubpass.pDepthStencilAttachment = &maskDepthRef;

        // Entry: wait for prior frame's fragment shader reads to finish
        // before this pass overwrites the color attachment.
        // Exit: let subsequent passes sample the color + depth results.
        VkSubpassDependency maskDeps[2]{};
        maskDeps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        maskDeps[0].dstSubpass    = 0;
        maskDeps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        maskDeps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        maskDeps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        // Depth is written at LATE_FRAGMENT_TESTS; cover it in the dst stage so the
        // DEPTH_STENCIL_ATTACHMENT_WRITE access has a matching stage.
        maskDeps[0].dstStageMask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        maskDeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        maskDeps[1].srcSubpass    = 0;
        maskDeps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        maskDeps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        maskDeps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        maskDeps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        maskDeps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        // No BY_REGION: outline shader reads a 3x3 mask neighborhood which can
        // cross framebuffer tiles on tiled GPUs.

        VkRenderPassCreateInfo maskRpInfo{};
        maskRpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        maskRpInfo.attachmentCount = 2;
        maskRpInfo.pAttachments    = maskAttachments;
        maskRpInfo.subpassCount    = 1;
        maskRpInfo.pSubpasses      = &maskSubpass;
        maskRpInfo.dependencyCount = 2;
        maskRpInfo.pDependencies   = maskDeps;
        if (maskPass_ == VK_NULL_HANDLE)  // persist across resize (see scenePass_)
            VK_CHECK(vkCreateRenderPass(ctx.device(), &maskRpInfo, nullptr, &maskPass_));

        // --- Mask framebuffer ---
        VkImageView maskViews[2] = {maskColorView_, maskDepthView_};
        VkFramebufferCreateInfo maskFbInfo{};
        maskFbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        maskFbInfo.renderPass      = maskPass_;
        maskFbInfo.attachmentCount = 2;
        maskFbInfo.pAttachments    = maskViews;
        maskFbInfo.width           = extent_.width;
        maskFbInfo.height          = extent_.height;
        maskFbInfo.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &maskFbInfo, nullptr, &maskFb_));
    }

    // -----------------------------------------------------------------------
    // Glow ping-pong scratch targets: two R16_SFLOAT images for separable blur.
    // H pass writes scratch[0]; V pass reads scratch[0] and writes scratch[1].
    // R16_SFLOAT is widely supported as a color attachment + sampled format and
    // is the natural choice for single-channel float coverage in [0,1].
    // -----------------------------------------------------------------------
    for (int i = 0; i < 2; ++i) {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = VK_FORMAT_R16_SFLOAT;
        iInfo.extent        = {extent_.width, extent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &glowScratch_[i], &glowScratchAlloc_[i], nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = glowScratch_[i];
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = VK_FORMAT_R16_SFLOAT;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &glowScratchView_[i]));
    }

    // --- Glow render pass: single R16_SFLOAT color attachment ---
    // loadOp = DONT_CARE (every pixel is overwritten by the full-screen triangle).
    // finalLayout = SHADER_READ_ONLY_OPTIMAL so the next pass can sample it.
    // Subpass dependencies mirror maskPass_: entry waits for prior sampling
    // to finish before writing; exit lets the next pass sample safely.
    {
        VkAttachmentDescription glowAttachment{};
        glowAttachment.format         = VK_FORMAT_R16_SFLOAT;
        glowAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        glowAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        glowAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        glowAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        glowAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        glowAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        glowAttachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference glowColorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription glowSubpass{};
        glowSubpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        glowSubpass.colorAttachmentCount = 1;
        glowSubpass.pColorAttachments    = &glowColorRef;

        VkSubpassDependency glowDeps[2]{};
        // Entry: wait for prior frame's sampling before writing color.
        glowDeps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        glowDeps[0].dstSubpass    = 0;
        glowDeps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        glowDeps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        glowDeps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        glowDeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // Exit: next pass can sample after color writes complete.
        glowDeps[1].srcSubpass      = 0;
        glowDeps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        glowDeps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        glowDeps[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        glowDeps[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        glowDeps[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        // No BY_REGION: glow V blur samples y-offset taps on scratch[0] which
        // cross framebuffer tiles; glow composite samples blurred scratch[1].

        VkRenderPassCreateInfo glowRpInfo{};
        glowRpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        glowRpInfo.attachmentCount = 1;
        glowRpInfo.pAttachments    = &glowAttachment;
        glowRpInfo.subpassCount    = 1;
        glowRpInfo.pSubpasses      = &glowSubpass;
        glowRpInfo.dependencyCount = 2;
        glowRpInfo.pDependencies   = glowDeps;
        if (glowPass_ == VK_NULL_HANDLE)  // persist across resize (see scenePass_)
            VK_CHECK(vkCreateRenderPass(ctx.device(), &glowRpInfo, nullptr, &glowPass_));

        // Two framebuffers — one per scratch image — both use the same glowPass_.
        for (int i = 0; i < 2; ++i) {
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = glowPass_;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments    = &glowScratchView_[i];
            fbInfo.width           = extent_.width;
            fbInfo.height          = extent_.height;
            fbInfo.layers          = 1;
            VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &glowFb_[i]));
        }
    }

    // M47 bloom mip-chain target (image + per-mip views/fbs). The pass is
    // created once and persisted (like glowPass_); see createBloomTargets.
    createBloomTargets(extent_);

    // M48 SSAO/blur targets (R8) + pass (persistent) + per-resize descriptor sets.
    createSsaoTargets(ctx, extent_);

    return true;
}

void VkPostProcess::destroyTargets(VkContext& ctx) {
    // M48 SSAO/blur image/views/fbs + per-resize sets (pass persists; freed in destroy()).
    destroySsaoTargets(ctx);

    // M47 bloom mip-chain image/views/fbs (pass persists; freed in destroy()).
    destroyBloomTargets();

    // Glow scratch targets (destroy in reverse creation order).
    // NOTE: glowPass_/maskPass_/scenePass_ are NOT destroyed here — render
    // passes are extent-independent and must persist across resize so the
    // pipelines built against them stay valid. They are torn down in destroy().
    for (int i = 1; i >= 0; --i) {
        if (glowFb_[i])          { vkDestroyFramebuffer(ctx.device(), glowFb_[i], nullptr); glowFb_[i] = VK_NULL_HANDLE; }
    }
    for (int i = 1; i >= 0; --i) {
        if (glowScratchView_[i]) { vkDestroyImageView(ctx.device(), glowScratchView_[i], nullptr); glowScratchView_[i] = VK_NULL_HANDLE; }
        if (glowScratch_[i])     { vmaDestroyImage(ctx.allocator(), glowScratch_[i], glowScratchAlloc_[i]); glowScratch_[i] = VK_NULL_HANDLE; glowScratchAlloc_[i] = VK_NULL_HANDLE; }
    }

    // Mask target (destroy before scene target — order doesn't matter to Vulkan,
    // but mirrors creation order in reverse for clarity).
    if (maskFb_)          { vkDestroyFramebuffer(ctx.device(), maskFb_, nullptr); maskFb_ = VK_NULL_HANDLE; }
    if (maskColorView_)   { vkDestroyImageView(ctx.device(), maskColorView_, nullptr); maskColorView_ = VK_NULL_HANDLE; }
    if (maskDepthView_)   { vkDestroyImageView(ctx.device(), maskDepthView_, nullptr); maskDepthView_ = VK_NULL_HANDLE; }
    if (maskColor_)       { vmaDestroyImage(ctx.allocator(), maskColor_, maskColorAlloc_); maskColor_ = VK_NULL_HANDLE; maskColorAlloc_ = VK_NULL_HANDLE; }
    if (maskDepth_)       { vmaDestroyImage(ctx.allocator(), maskDepth_, maskDepthAlloc_); maskDepth_ = VK_NULL_HANDLE; maskDepthAlloc_ = VK_NULL_HANDLE; }

    // Scene offscreen target.
    if (sceneFb_)         { vkDestroyFramebuffer(ctx.device(), sceneFb_, nullptr); sceneFb_ = VK_NULL_HANDLE; }
    if (sceneColorView_)  { vkDestroyImageView(ctx.device(), sceneColorView_, nullptr); sceneColorView_ = VK_NULL_HANDLE; }
    if (sceneDepthView_)  { vkDestroyImageView(ctx.device(), sceneDepthView_, nullptr); sceneDepthView_ = VK_NULL_HANDLE; }
    if (sceneColor_)      { vmaDestroyImage(ctx.allocator(), sceneColor_, sceneColorAlloc_); sceneColor_ = VK_NULL_HANDLE; sceneColorAlloc_ = VK_NULL_HANDLE; }
    if (sceneDepth_)      { vmaDestroyImage(ctx.allocator(), sceneDepth_, sceneDepthAlloc_); sceneDepth_ = VK_NULL_HANDLE; sceneDepthAlloc_ = VK_NULL_HANDLE; }
}

// ---------------------------------------------------------------------------
// M47 bloom — mip-chain target, render pass, and 3 pipelines.
// ---------------------------------------------------------------------------

std::uint32_t VkPostProcess::computeBloomMipCount(VkExtent2D extent) {
    std::uint32_t s = std::min(extent.width, extent.height) / 2u;  // mip0 = half res
    std::uint32_t n = 1;
    while (s > 8u && n < kBloomMaxMips) { s >>= 1u; ++n; }
    return n;  // in [1, kBloomMaxMips]
}

// Creates a single RGBA16F image (mip0 = half scene res) with per-mip image
// views + framebuffers, plus a render pass shared by all mip framebuffers.
// The image/views/fbs are per-resize (rebuilt in createTargets); the pass is
// created once and persists (pipelines are built against it — see glowPass_).
// All mips are transitioned UNDEFINED -> SHADER_READ_ONLY_OPTIMAL once here so
// they satisfy bloomPass_'s initialLayout = SHADER_READ_ONLY before first use.
void VkPostProcess::createBloomTargets(VkExtent2D extent) {
    VkContext& ctx = *ctx_;
    bloomMipCount_ = computeBloomMipCount(extent);

    const std::uint32_t w0 = std::max(1u, extent.width  / 2u);
    const std::uint32_t h0 = std::max(1u, extent.height / 2u);

    // Single image with bloomMipCount_ mip levels.
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        iInfo.extent        = {w0, h0, 1};
        iInfo.mipLevels     = bloomMipCount_;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &bloomChain_, &bloomChainAlloc_, nullptr));
    }

    // Per-mip views + extents.
    for (std::uint32_t m = 0; m < bloomMipCount_; ++m) {
        bloomMipExtents_[m] = {std::max(1u, w0 >> m), std::max(1u, h0 >> m)};

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                       = bloomChain_;
        vInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                      = VK_FORMAT_R16G16B16A16_SFLOAT;
        vInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.baseMipLevel = m;
        vInfo.subresourceRange.levelCount   = 1;
        vInfo.subresourceRange.layerCount   = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &bloomMipViews_[m]));
    }

    // Render pass: single RGBA16F color attachment, loadOp = LOAD so the
    // additive upsample preserves the mip's existing (downsampled) content and
    // adds onto it; initialLayout = finalLayout = SHADER_READ_ONLY_OPTIMAL so a
    // mip written as an attachment is immediately sampleable by the next pass.
    // Two subpass dependencies mirror glowPass_ (FRAGMENT_SHADER <-> COLOR_OUT).
    if (bloomPass_ == VK_NULL_HANDLE) {  // persist across resize (see glowPass_)
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_R16G16B16A16_SFLOAT;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        VkSubpassDependency deps[2]{};
        // Entry: wait for prior sampling (and, with LOAD, the read of existing
        // attachment contents) before writing color.
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // Exit: next pass can sample after color writes complete.
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments    = &att;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 2;
        rpInfo.pDependencies   = deps;
        VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &bloomPass_));
    }

    // Per-mip framebuffers (each binds its own mip view, sized to that mip).
    for (std::uint32_t m = 0; m < bloomMipCount_; ++m) {
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = bloomPass_;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &bloomMipViews_[m];
        fbInfo.width           = bloomMipExtents_[m].width;
        fbInfo.height          = bloomMipExtents_[m].height;
        fbInfo.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &bloomMipFbs_[m]));
    }

    // One-shot barrier: transition ALL mips UNDEFINED -> SHADER_READ_ONLY_OPTIMAL
    // so each mip is in bloomPass_'s initialLayout before its first pass. Mirrors
    // VkIblBaker's transient command pool + queue-wait-idle pattern.
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pInfo{};
        pInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pInfo.queueFamilyIndex = ctx.graphicsFamily();
        pInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VK_CHECK(vkCreateCommandPool(ctx.device(), &pInfo, nullptr, &pool));

        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbInfo{};
        cbInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbInfo.commandPool        = pool;
        cbInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb));

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &begin));

        VkImageMemoryBarrier mb{};
        mb.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mb.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        mb.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image               = bloomChain_;
        mb.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, bloomMipCount_, 0, 1};
        mb.srcAccessMask       = 0;
        mb.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &mb);

        VK_CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cb;
        VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));
        vkDestroyCommandPool(ctx.device(), pool, nullptr);
    }

    // The descriptor set layout (binding 0 = sampler2D) is persistent. It is
    // created here — createBloomTargets runs before createBloomPipelines in init
    // (and is the first user of the layout, allocating the per-source sets below).
    if (bloomSetLayout_ == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding slb{};
        slb.binding         = 0;
        slb.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        slb.descriptorCount = 1;
        slb.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slInfo{};
        slInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        slInfo.bindingCount = 1;
        slInfo.pBindings    = &slb;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &slInfo, nullptr, &bloomSetLayout_));
    }

    // Persistent per-source descriptor sets (mirrors glow's pre-written sets).
    // The shared descPool_ is full (maxSets=7), so bloom owns a dedicated pool
    // sized for the worst case: 1 bright set (scene) + one set per mip. Sources
    // are fixed for the lifetime of these views, so we write the sets once here.
    {
        const std::uint32_t setCount = 1u + bloomMipCount_;  // bright + per-mip

        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = setCount;  // 1 sampler per set

        VkDescriptorPoolCreateInfo dpInfo{};
        dpInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpInfo.maxSets       = setCount;
        dpInfo.poolSizeCount = 1;
        dpInfo.pPoolSizes    = &poolSize;
        VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpInfo, nullptr, &bloomDescPool_));

        // Allocate the bright set (samples sceneColorView_).
        {
            VkDescriptorSetAllocateInfo dsAlloc{};
            dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsAlloc.descriptorPool     = bloomDescPool_;
            dsAlloc.descriptorSetCount = 1;
            dsAlloc.pSetLayouts        = &bloomSetLayout_;
            VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, &bloomBrightSet_));

            VkDescriptorImageInfo imgInfo{};
            imgInfo.sampler     = bloomSampler_;
            imgInfo.imageView   = sceneColorView_;
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet write{};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = bloomBrightSet_;
            write.dstBinding      = 0;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo      = &imgInfo;
            vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
        }

        // Allocate one set per mip, each sampling bloomMipViews_[m].
        for (std::uint32_t m = 0; m < bloomMipCount_; ++m) {
            VkDescriptorSetAllocateInfo dsAlloc{};
            dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsAlloc.descriptorPool     = bloomDescPool_;
            dsAlloc.descriptorSetCount = 1;
            dsAlloc.pSetLayouts        = &bloomSetLayout_;
            VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, &bloomSrcSets_[m]));

            VkDescriptorImageInfo imgInfo{};
            imgInfo.sampler     = bloomSampler_;
            imgInfo.imageView   = bloomMipViews_[m];
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet write{};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = bloomSrcSets_[m];
            write.dstBinding      = 0;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo      = &imgInfo;
            vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
        }
    }
}

void VkPostProcess::destroyBloomTargets() {
    VkContext& ctx = *ctx_;
    // Per-resize handles only. bloomPass_ persists (freed in destroy()).
    // The dedicated bloom pool owns the per-source sets; destroying it frees them.
    if (bloomDescPool_) {
        vkDestroyDescriptorPool(ctx.device(), bloomDescPool_, nullptr);
        bloomDescPool_  = VK_NULL_HANDLE;
        bloomBrightSet_ = VK_NULL_HANDLE;
        for (std::uint32_t m = 0; m < kBloomMaxMips; ++m) bloomSrcSets_[m] = VK_NULL_HANDLE;
    }
    for (std::uint32_t m = 0; m < kBloomMaxMips; ++m) {
        if (bloomMipFbs_[m])   { vkDestroyFramebuffer(ctx.device(), bloomMipFbs_[m], nullptr); bloomMipFbs_[m] = VK_NULL_HANDLE; }
    }
    for (std::uint32_t m = 0; m < kBloomMaxMips; ++m) {
        if (bloomMipViews_[m]) { vkDestroyImageView(ctx.device(), bloomMipViews_[m], nullptr); bloomMipViews_[m] = VK_NULL_HANDLE; }
        bloomMipExtents_[m] = {};
    }
    if (bloomChain_) { vmaDestroyImage(ctx.allocator(), bloomChain_, bloomChainAlloc_); bloomChain_ = VK_NULL_HANDLE; bloomChainAlloc_ = VK_NULL_HANDLE; }
    bloomMipCount_ = 0;
}

bool VkPostProcess::createBloomPipelines() {
    VkContext& ctx = *ctx_;

    // Compile the shared full-screen vertex shader once; reuse for all 3 passes.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kFullscreenVert);
    if (vspv.empty()) {
        Log::error("VkPostProcess: bloom vertex shader compile failed");
        return false;
    }

    auto makeModule = [&](const std::vector<std::uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo smInfo{};
        smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smInfo.codeSize = spv.size() * sizeof(std::uint32_t);
        smInfo.pCode    = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &m) != VK_SUCCESS)
            m = VK_NULL_HANDLE;
        return m;
    };

    // bloomSetLayout_ (binding 0 = sampler2D) is already created by
    // createBloomTargets, which runs before this in init. Just use it here.

    // Common full-screen pipeline state (mirrors createGlowPipelines).
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1;
    vpState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex bindings — positions come from gl_VertexIndex.

    // Blend states: down/bright-down overwrite; up is additive (ONE,ONE,ADD).
    VkPipelineColorBlendAttachmentState overwriteAtt{};
    overwriteAtt.colorWriteMask = 0xF;  // RGBA
    overwriteAtt.blendEnable    = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo overwriteBlend{};
    overwriteBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    overwriteBlend.attachmentCount = 1;
    overwriteBlend.pAttachments    = &overwriteAtt;

    VkPipelineColorBlendAttachmentState additiveAtt{};
    additiveAtt.colorWriteMask         = 0xF;  // RGBA
    additiveAtt.blendEnable            = VK_TRUE;
    additiveAtt.srcColorBlendFactor    = VK_BLEND_FACTOR_ONE;
    additiveAtt.dstColorBlendFactor    = VK_BLEND_FACTOR_ONE;
    additiveAtt.colorBlendOp           = VK_BLEND_OP_ADD;
    additiveAtt.srcAlphaBlendFactor    = VK_BLEND_FACTOR_ONE;
    additiveAtt.dstAlphaBlendFactor    = VK_BLEND_FACTOR_ONE;
    additiveAtt.alphaBlendOp           = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo additiveBlend{};
    additiveBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    additiveBlend.attachmentCount = 1;
    additiveBlend.pAttachments    = &additiveAtt;

    // Helper to build one bloom pipeline: own push-constant range + layout +
    // graphics pipeline against bloomPass_ using kFullscreenVert + `frag`.
    auto buildPipeline = [&](const char* frag, std::uint32_t pcSize,
                             const VkPipelineColorBlendStateCreateInfo& blend,
                             VkPipelineLayout* outLayout, ::VkPipeline* outPipe,
                             const char* tag) -> bool {
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset     = 0;
        pcRange.size       = pcSize;

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &bloomSetLayout_;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &pcRange;
        VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, outLayout));

        auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, frag);
        if (fspv.empty()) { Log::error("VkPostProcess: bloom %s shader compile failed", tag); return false; }

        VkShaderModule vsm = makeModule(vspv);
        VkShaderModule fsm = makeModule(fspv);
        if (!vsm || !fsm) {
            if (vsm) vkDestroyShaderModule(ctx.device(), vsm, nullptr);
            if (fsm) vkDestroyShaderModule(ctx.device(), fsm, nullptr);
            Log::error("VkPostProcess: bloom %s shader module creation failed", tag);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vsm; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fsm; stages[1].pName = "main";

        VkGraphicsPipelineCreateInfo pInfo{};
        pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pInfo.stageCount          = 2; pInfo.pStages = stages;
        pInfo.pVertexInputState   = &vi;
        pInfo.pInputAssemblyState = &ia;
        pInfo.pViewportState      = &vpState;
        pInfo.pRasterizationState = &rs;
        pInfo.pMultisampleState   = &ms;
        pInfo.pDepthStencilState  = &ds;
        pInfo.pColorBlendState    = &blend;
        pInfo.pDynamicState       = &dyn;
        pInfo.layout              = *outLayout;
        pInfo.renderPass          = bloomPass_;  // offscreen RGBA16F mip pass
        pInfo.subpass             = 0;
        VkResult r = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, outPipe);
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        vkDestroyShaderModule(ctx.device(), fsm, nullptr);
        VK_CHECK(r);
        return true;
    };

    // bright-down (scene -> mip0): overwrite blend.
    if (!buildPipeline(kBloomPrefilterDownSrc(), sizeof(BloomBrightPush),
                       overwriteBlend, &bloomBrightDownLayout_, &bloomBrightDownPipeline_, "bright-down")) return false;
    // down (mip[i] -> mip[i+1]): overwrite blend.
    if (!buildPipeline(kBloomDownsampleSrc(), sizeof(BloomDownPush),
                       overwriteBlend, &bloomDownLayout_, &bloomDownPipeline_, "down")) return false;
    // up (mip[i+1] -> mip[i]): additive blend.
    if (!buildPipeline(kBloomUpsampleSrc(), sizeof(BloomUpPush),
                       additiveBlend, &bloomUpLayout_, &bloomUpPipeline_, "up")) return false;
    return true;
}

bool VkPostProcess::createViewportTarget(VkContext& ctx) {
    // --- Viewport color image (colorFormat_, COLOR_ATTACHMENT | SAMPLED) ---
    // Uses the same format as the swapchain color so pipelines built against
    // the swapchain render pass are format-compatible with viewportPass_.
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = colorFormat_;
        iInfo.extent        = {viewportExtent_.width, viewportExtent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &viewportColor_, &viewportColorAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = viewportColor_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = colorFormat_;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &viewportColorView_));
    }

    // --- Viewport depth image (depthFormat_, DEPTH_STENCIL_ATTACHMENT only) ---
    // No SAMPLED_BIT — nothing samples viewport depth; the attachment is
    // present only for depth-tested draws (debug lines, HUD) recording into this pass.
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = depthFormat_;
        iInfo.extent        = {viewportExtent_.width, viewportExtent_.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &viewportDepth_, &viewportDepthAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image                           = viewportDepth_;
        vInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format                          = depthFormat_;
        vInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        vInfo.subresourceRange.levelCount     = 1;
        vInfo.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &viewportDepthView_));
    }

    // --- Viewport render pass: color + depth, format-compatible with swapchain ---
    // Color finalLayout = SHADER_READ_ONLY_OPTIMAL so the swapchain blit pass
    // (Task 2) can sample it as a texture. Depth is DONT_CARE on store since
    // nothing samples viewport depth.
    // Two subpass dependencies (entry + color exit), mirroring the scene pass's
    // first two deps (the scene pass adds a third for sampleable depth — not
    // needed here since viewport depth is not sampled).
    {
        VkAttachmentDescription attachments[2]{};
        // Color attachment: swapchain color format, cleared + stored, transitions
        // to SHADER_READ_ONLY_OPTIMAL so the blit pipeline can sample it.
        attachments[0].format         = colorFormat_;
        attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // Depth attachment: swapchain depth format, cleared, not stored (not sampled).
        attachments[1].format         = depthFormat_;
        attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency deps[2]{};
        // Entry: wait for prior frame's sampling to finish before writing color + depth.
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // Exit (color): blit/sample pass can read color after color writes complete.
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 2;
        rpInfo.pAttachments    = attachments;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 2;
        rpInfo.pDependencies   = deps;
        if (viewportPass_ == VK_NULL_HANDLE)  // persist across resize (see scenePass_)
            VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &viewportPass_));

        // --- Viewport framebuffer ---
        VkImageView views[2] = {viewportColorView_, viewportDepthView_};
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = viewportPass_;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments    = views;
        fbInfo.width           = viewportExtent_.width;
        fbInfo.height          = viewportExtent_.height;
        fbInfo.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &viewportFb_));
    }

    return true;
}

void VkPostProcess::destroyViewportTarget(VkContext& ctx) {
    // viewportPass_ is NOT destroyed here — it persists across resize so the
    // composite/debug/HUD pipelines built against it stay valid (torn down in
    // destroy()).
    if (viewportFb_)         { vkDestroyFramebuffer(ctx.device(), viewportFb_, nullptr); viewportFb_ = VK_NULL_HANDLE; }
    if (viewportColorView_)  { vkDestroyImageView(ctx.device(), viewportColorView_, nullptr); viewportColorView_ = VK_NULL_HANDLE; }
    if (viewportDepthView_)  { vkDestroyImageView(ctx.device(), viewportDepthView_, nullptr); viewportDepthView_ = VK_NULL_HANDLE; }
    if (viewportColor_)      { vmaDestroyImage(ctx.allocator(), viewportColor_, viewportColorAlloc_); viewportColor_ = VK_NULL_HANDLE; viewportColorAlloc_ = VK_NULL_HANDLE; }
    if (viewportDepth_)      { vmaDestroyImage(ctx.allocator(), viewportDepth_, viewportDepthAlloc_); viewportDepth_ = VK_NULL_HANDLE; viewportDepthAlloc_ = VK_NULL_HANDLE; }
}

bool VkPostProcess::resizeViewport(VkContext& ctx, VkExtent2D extent) {
    if (extent.width == 0 || extent.height == 0) return true;
    if (extent.width == viewportExtent_.width &&
        extent.height == viewportExtent_.height) return true;
    vkDeviceWaitIdle(ctx.device());
    destroyViewportTarget(ctx);
    viewportExtent_ = extent;
    if (!createViewportTarget(ctx)) return false;

    // Re-point the blit descriptor at the new viewport image.
    if (blitDescSet_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = sampler_;
        imgInfo.imageView   = viewportColorView_;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = blitDescSet_;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }
    return true;
}

bool VkPostProcess::createCopyPipeline(VkContext& ctx) {
    // Compile shaders. The composite pass uses kCompositeFrag (bloom-aware,
    // 2 bindings); the viewport->swapchain blit keeps the binding-0-only kCopyFrag.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   kFullscreenVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kCompositeFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkPostProcess: shader compile failed");
        return false;
    }

    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkPostProcess: fragment shader module creation failed");
        return false;
    }

    // Descriptor set layout: binding 0 = scene color, binding 1 = bloom mip0,
    // binding 2 = blurred SSAO (all combined image samplers, FS). (M47/M48)
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 3;
    dslInfo.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &copySetLayout_));

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(CopyPush);

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &copySetLayout_;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &copyPipeLayout_));

    // Pipeline — full-screen triangle, no vertex input, depth test off.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex bindings or attributes — positions come from gl_VertexIndex.

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
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount          = 2;
    pInfo.pStages             = stages;
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = copyPipeLayout_;
    pInfo.renderPass          = viewportPass_;  // composite records into viewportPass_
    pInfo.subpass             = 0;
    // Capture result so shader modules are destroyed even if pipeline creation fails.
    VkResult copyPipeResult = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                                        &pInfo, nullptr, &copyPipeline_);
    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    VK_CHECK(copyPipeResult);

    // Descriptor pool: 7 sets total, 12 combined-image-sampler descriptors.
    //   set 0: copy/composite — 3 samplers (scene color + bloom mip0 + SSAO)  (M47/M48)
    //   set 1: blit         — 1 sampler  (viewport color)
    //   set 2: outline      — 1 sampler  (mask)  (overlay: scene from Copy base)
    //   set 3: glowBlurH    — 1 sampler  (mask)
    //   set 4: glowBlurV    — 1 sampler  (scratch[0])
    //   set 5: glowComposite — 2 samplers (scratch[1] + mask)  (overlay: scene from Copy base)
    //   set 6: xray         — 3 samplers (mask id + mask depth + scene depth)  (overlay: scene from Copy base)
    // Total: 7 sets, 12 combined-image-samplers.
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 12;

    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets       = 7;
    dpInfo.poolSizeCount = 1;
    dpInfo.pPoolSizes    = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpInfo, nullptr, &descPool_));

    // Allocate and write the copy descriptor set.
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = descPool_;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &copySetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, &copyDescSet_));

    // Binding 0 = scene color, binding 1 = bloom mip0, binding 2 = blurred SSAO.
    // bloomMipViews_[0] and ssaoBlurView_ are valid here: init() runs
    // createTargets() (which calls createBloomTargets + createSsaoTargets) BEFORE
    // createCopyPipeline(). ssaoBlurView_ uses bloomSampler_ (LINEAR + CLAMP). (M47/M48)
    VkDescriptorImageInfo imgInfos[3]{};
    imgInfos[0].sampler     = sampler_;
    imgInfos[0].imageView   = sceneColorView_;
    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[1].sampler     = bloomSampler_;
    imgInfos[1].imageView   = bloomMipViews_[0];
    imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[2].sampler     = bloomSampler_;
    imgInfos[2].imageView   = ssaoBlurView_;
    imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = copyDescSet_;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &imgInfos[0];
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = copyDescSet_;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &imgInfos[1];
    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = copyDescSet_;
    writes[2].dstBinding      = 2;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo      = &imgInfos[2];
    vkUpdateDescriptorSets(ctx.device(), 3, writes, 0, nullptr);

    return true;
}

bool VkPostProcess::createBlitPipeline(VkContext& ctx, VkRenderPass swapchainPass) {
    // Compile shaders (same kFullscreenVert + kCopyFrag as the copy pipeline).
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   kFullscreenVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kCopyFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkPostProcess: blit shader compile failed");
        return false;
    }

    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkPostProcess: blit fragment shader module creation failed");
        return false;
    }

    // Descriptor set layout: binding 0 = combined image sampler (FS).
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings    = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &blitSetLayout_));

    // kCopyFrag declares `layout(push_constant) uniform Push { float exposure; }`,
    // so the layout must include a FRAGMENT push-constant range covering that block.
    // Without it, vkCreateGraphicsPipelines raises VUID-07987. The blit path
    // (viewport→swapchain) never actually pushes constants at draw time; the range
    // here only satisfies the layout/shader-compatibility requirement.
    VkPushConstantRange blitPcRange{};
    blitPcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    blitPcRange.offset     = 0;
    blitPcRange.size       = sizeof(float);  // kCopyFrag Push: { float exposure; }

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &blitSetLayout_;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &blitPcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &blitPipeLayout_));

    // Pipeline — full-screen triangle, no vertex input, depth test off.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex bindings or attributes — positions come from gl_VertexIndex.

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
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount          = 2;
    pInfo.pStages             = stages;
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = blitPipeLayout_;
    pInfo.renderPass          = swapchainPass;
    pInfo.subpass             = 0;
    // Capture result so shader modules are destroyed even if pipeline creation fails.
    VkResult blitPipeResult = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                                        &pInfo, nullptr, &blitPipeline_);
    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    VK_CHECK(blitPipeResult);

    // Allocate and write the blit descriptor set (from the shared descPool_).
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = descPool_;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &blitSetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, &blitDescSet_));

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = sampler_;
    imgInfo.imageView   = viewportColorView_;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = blitDescSet_;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);

    return true;
}

bool VkPostProcess::createMaskPipeline(VkContext& ctx) {
    // Push-constant range: one range covering both stages (vertex uses mvp,
    // fragment uses id). Size = sizeof(MaskPushConstants) = 64 + 4 = 68 bytes.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = static_cast<uint32_t>(sizeof(MaskPushConstants));

    // No descriptor sets — push-constant only.
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 0;
    plInfo.pSetLayouts            = nullptr;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &maskPipeLayout_));

    // Compile shaders.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   kMaskVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kMaskFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkPostProcess: mask shader compile failed");
        return false;
    }

    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkPostProcess: mask fragment shader module creation failed");
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    // Vertex input: full Vertex struct stride (44 bytes: Vec3+Vec3+Vec2+Vec3),
    // only position (location 0) declared — mirrors VkShadowMap exactly so
    // binding the normal mesh vertex buffer works without re-uploading.
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = offsetof(Vertex, position);
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = offsetof(Vertex, normal);
    attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32G32_SFLOAT;    attrs[2].offset = offsetof(Vertex, uv);
    attrs[3].location = 3; attrs[3].binding = 0; attrs[3].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[3].offset = offsetof(Vertex, tangent);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions    = attrs;

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
    rs.cullMode    = VK_CULL_MODE_BACK_BIT;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    // Single R8_UINT color attachment — no blending (integers don't blend).
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    att.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount          = 2;
    pInfo.pStages             = stages;
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = maskPipeLayout_;
    pInfo.renderPass          = maskPass_;
    pInfo.subpass             = 0;
    // Capture result so shader modules are destroyed even if pipeline creation fails.
    VkResult maskPipeResult = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                                        &pInfo, nullptr, &maskPipeline_);
    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    VK_CHECK(maskPipeResult);

    return true;
}

bool VkPostProcess::createOutlinePipeline(VkContext& ctx) {
    // Compile shaders (reuse kFullscreenVert from the copy pipeline).
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   kFullscreenVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kOutlineFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkPostProcess: outline shader compile failed");
        return false;
    }

    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkPostProcess: outline fragment shader module creation failed");
        return false;
    }

    // Descriptor set layout: binding 0 = usampler2D (mask id). Overlay — the scene
    // comes from the Copy base, so no scene sampler. COMBINED_IMAGE_SAMPLER; the mask
    // uses maskSampler_ (NEAREST) at runtime (integer texture cannot be linear-filtered).
    VkDescriptorSetLayoutBinding bindings[1]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &outlineSetLayout_));

    // Push constant for outline params (fragment stage).
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = static_cast<uint32_t>(sizeof(OutlinePush));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &outlineSetLayout_;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &outlinePipeLayout_));

    // Pipeline — full-screen triangle, no vertex input, depth test off.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex bindings — positions come from gl_VertexIndex.

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
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    // Alpha-blend the outline color over the composited Copy base.
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable         = VK_TRUE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.colorBlendOp        = VK_BLEND_OP_ADD;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    att.alphaBlendOp        = VK_BLEND_OP_ADD;
    att.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT;  // RGB only: preserve the
    // viewport alpha (Copy wrote a=1; the editor's ImGui::Image samples it, so
    // zeroing alpha here would make the scene render transparent).

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount          = 2;
    pInfo.pStages             = stages;
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = outlinePipeLayout_;
    pInfo.renderPass          = viewportPass_;  // composite records into viewportPass_
    pInfo.subpass             = 0;
    // Capture result so shader modules are destroyed even if pipeline creation fails.
    VkResult outlinePipeResult = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE,
                                                           1, &pInfo, nullptr,
                                                           &outlinePipeline_);
    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    VK_CHECK(outlinePipeResult);

    // Allocate the outline descriptor set from the shared pool (allocated in
    // createCopyPipeline with maxSets=7, descriptorCount=12).
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = descPool_;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &outlineSetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, &outlineDescSet_));

    // Write the outline descriptor set: binding 0 = mask id (NEAREST sampler —
    // integer texture cannot be linear-filtered). Overlay: no scene sampler.
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = maskSampler_;
    imgInfo.imageView   = maskColorView_;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = outlineDescSet_;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);

    return true;
}

bool VkPostProcess::createGlowPipelines(VkContext& ctx) {
    // Compile the vertex shader (shared kFullscreenVert) once; reuse for all three passes.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kFullscreenVert);
    if (vspv.empty()) {
        Log::error("VkPostProcess: glow vertex shader compile failed");
        return false;
    }

    // Helper lambda: create a VkShaderModule from a SPIR-V vector.
    auto makeModule = [&](const std::vector<std::uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo smInfo{};
        smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smInfo.codeSize = spv.size() * sizeof(std::uint32_t);
        smInfo.pCode    = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &m) != VK_SUCCESS)
            m = VK_NULL_HANDLE;
        return m;
    };

    // Common full-screen pipeline state (reused for all three passes).
    // We fill pInfo per-pipeline; only renderPass and layout differ.
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1;
    vpState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    // ADDITIVE (ONE/ONE) blend — used ONLY by the GlowComposite pipeline (the
    // blur pipelines use their own single-channel blend-OFF state). The halo adds
    // on top of the Copy composite base.
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.blendEnable         = VK_TRUE;
    blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.colorBlendOp        = VK_BLEND_OP_ADD;
    blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendAtt.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT;  // RGB only: preserve viewport alpha (Copy's a=1)

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blendAtt;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex bindings — positions come from gl_VertexIndex.

    // Push-constant range used by all glow passes (fragment stage only).
    // GlowBlurPush (16 bytes) and GlowCompositePush (32 bytes) — declare
    // the larger so both fit within one layout's range.
    VkPushConstantRange blurPcRange{};
    blurPcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    blurPcRange.offset     = 0;
    blurPcRange.size       = static_cast<uint32_t>(sizeof(GlowBlurPush));

    VkPushConstantRange compositePcRange{};
    compositePcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    compositePcRange.offset     = 0;
    compositePcRange.size       = static_cast<uint32_t>(sizeof(GlowCompositePush));

    // -----------------------------------------------------------------
    // GlowBlurH pipeline: reads mask (usampler2D, NEAREST) -> writes R16_SFLOAT scratch[0].
    // Descriptor set layout: binding 0 = usampler2D uMask.
    // -----------------------------------------------------------------
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 1;
        dslInfo.pBindings    = &b;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &glowBlurHSetLayout_));

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &glowBlurHSetLayout_;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &blurPcRange;
        VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &glowBlurHPipeLayout_));

        auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kGlowBlurHFrag);
        if (fspv.empty()) { Log::error("VkPostProcess: glow blur-H shader compile failed"); return false; }

        VkShaderModule vsm = makeModule(vspv);
        VkShaderModule fsm = makeModule(fspv);
        if (!vsm || !fsm) {
            if (vsm) vkDestroyShaderModule(ctx.device(), vsm, nullptr);
            if (fsm) vkDestroyShaderModule(ctx.device(), fsm, nullptr);
            Log::error("VkPostProcess: glow blur-H shader module creation failed");
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vsm; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fsm; stages[1].pName = "main";

        // BlurH writes a single R16_SFLOAT output — single-channel blend state.
        VkPipelineColorBlendAttachmentState singleAtt{};
        singleAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        singleAtt.blendEnable    = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo singleBlend{};
        singleBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        singleBlend.attachmentCount = 1;
        singleBlend.pAttachments    = &singleAtt;

        VkGraphicsPipelineCreateInfo pInfo{};
        pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pInfo.stageCount          = 2; pInfo.pStages = stages;
        pInfo.pVertexInputState   = &vi;
        pInfo.pInputAssemblyState = &ia;
        pInfo.pViewportState      = &vpState;
        pInfo.pRasterizationState = &rs;
        pInfo.pMultisampleState   = &ms;
        pInfo.pDepthStencilState  = &ds;
        pInfo.pColorBlendState    = &singleBlend;
        pInfo.pDynamicState       = &dyn;
        pInfo.layout              = glowBlurHPipeLayout_;
        pInfo.renderPass          = glowPass_;  // offscreen R16_SFLOAT pass
        pInfo.subpass             = 0;
        VkResult r = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &glowBlurHPipeline_);
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        vkDestroyShaderModule(ctx.device(), fsm, nullptr);
        VK_CHECK(r);
    }

    // -----------------------------------------------------------------
    // GlowBlurV pipeline: reads scratch[0] (sampler2D, linear) -> writes scratch[1].
    // Descriptor set layout: binding 0 = sampler2D uCov.
    // -----------------------------------------------------------------
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 1;
        dslInfo.pBindings    = &b;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &glowBlurVSetLayout_));

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &glowBlurVSetLayout_;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &blurPcRange;
        VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &glowBlurVPipeLayout_));

        auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kGlowBlurVFrag);
        if (fspv.empty()) { Log::error("VkPostProcess: glow blur-V shader compile failed"); return false; }

        VkShaderModule vsm = makeModule(vspv);
        VkShaderModule fsm = makeModule(fspv);
        if (!vsm || !fsm) {
            if (vsm) vkDestroyShaderModule(ctx.device(), vsm, nullptr);
            if (fsm) vkDestroyShaderModule(ctx.device(), fsm, nullptr);
            Log::error("VkPostProcess: glow blur-V shader module creation failed");
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vsm; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fsm; stages[1].pName = "main";

        VkPipelineColorBlendAttachmentState singleAtt{};
        singleAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        singleAtt.blendEnable    = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo singleBlend{};
        singleBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        singleBlend.attachmentCount = 1;
        singleBlend.pAttachments    = &singleAtt;

        VkGraphicsPipelineCreateInfo pInfo{};
        pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pInfo.stageCount          = 2; pInfo.pStages = stages;
        pInfo.pVertexInputState   = &vi;
        pInfo.pInputAssemblyState = &ia;
        pInfo.pViewportState      = &vpState;
        pInfo.pRasterizationState = &rs;
        pInfo.pMultisampleState   = &ms;
        pInfo.pDepthStencilState  = &ds;
        pInfo.pColorBlendState    = &singleBlend;
        pInfo.pDynamicState       = &dyn;
        pInfo.layout              = glowBlurVPipeLayout_;
        pInfo.renderPass          = glowPass_;  // offscreen R16_SFLOAT pass
        pInfo.subpass             = 0;
        VkResult r = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &glowBlurVPipeline_);
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        vkDestroyShaderModule(ctx.device(), fsm, nullptr);
        VK_CHECK(r);
    }

    // -----------------------------------------------------------------
    // GlowComposite pipeline (overlay): reads scratch[1] + mask; writes viewport
    // pass (M43a). Built against viewportPass_ for render-pass compatibility.
    // The scene comes from the Copy base, so no scene sampler — the halo blends
    // additively (ONE/ONE) on top.
    // Descriptor set layout: binding 0 = sampler2D uBlur,
    //                        binding 1 = usampler2D uMask.
    // -----------------------------------------------------------------
    {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0; bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[0].descriptorCount = 1; bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1; bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[1].descriptorCount = 1; bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 2;
        dslInfo.pBindings    = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &glowCompositeSetLayout_));

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &glowCompositeSetLayout_;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &compositePcRange;
        VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &glowCompositePipeLayout_));

        auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kGlowCompositeFrag);
        if (fspv.empty()) { Log::error("VkPostProcess: glow composite shader compile failed"); return false; }

        VkShaderModule vsm = makeModule(vspv);
        VkShaderModule fsm = makeModule(fspv);
        if (!vsm || !fsm) {
            if (vsm) vkDestroyShaderModule(ctx.device(), vsm, nullptr);
            if (fsm) vkDestroyShaderModule(ctx.device(), fsm, nullptr);
            Log::error("VkPostProcess: glow composite shader module creation failed");
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vsm; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fsm; stages[1].pName = "main";

        VkGraphicsPipelineCreateInfo pInfo{};
        pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pInfo.stageCount          = 2; pInfo.pStages = stages;
        pInfo.pVertexInputState   = &vi;
        pInfo.pInputAssemblyState = &ia;
        pInfo.pViewportState      = &vpState;
        pInfo.pRasterizationState = &rs;
        pInfo.pMultisampleState   = &ms;
        pInfo.pDepthStencilState  = &ds;
        pInfo.pColorBlendState    = &blend;  // RGB-only write mask — overlay preserves viewport alpha (Copy's a=1)
        pInfo.pDynamicState       = &dyn;
        pInfo.layout              = glowCompositePipeLayout_;
        pInfo.renderPass          = viewportPass_;  // composite records into viewportPass_ (must match its dependencyCount)
        pInfo.subpass             = 0;
        VkResult r = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &glowCompositePipeline_);
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        vkDestroyShaderModule(ctx.device(), fsm, nullptr);
        VK_CHECK(r);
    }

    // -----------------------------------------------------------------
    // Allocate the three glow descriptor sets from the shared pool.
    // -----------------------------------------------------------------
    {
        VkDescriptorSetLayout layouts[3] = {
            glowBlurHSetLayout_,
            glowBlurVSetLayout_,
            glowCompositeSetLayout_,
        };
        VkDescriptorSet sets[3] = {};
        VkDescriptorSetAllocateInfo dsAlloc{};
        dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool     = descPool_;
        dsAlloc.descriptorSetCount = 3;
        dsAlloc.pSetLayouts        = layouts;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, sets));
        glowBlurHDescSet_     = sets[0];
        glowBlurVDescSet_     = sets[1];
        glowCompositeDescSet_ = sets[2];
    }

    // Write initial descriptor values (same views as resize would write).
    // GlowBlurH: binding 0 = maskColorView_ + maskSampler_ (NEAREST — integer texture).
    {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = maskSampler_;
        imgInfo.imageView   = maskColorView_;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = glowBlurHDescSet_; write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1; write.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }
    // GlowBlurV: binding 0 = glowScratchView_[0] + sampler_ (linear).
    {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = sampler_;
        imgInfo.imageView   = glowScratchView_[0];
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = glowBlurVDescSet_; write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1; write.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }
    // GlowComposite (overlay — no scene sampler): binding 0 = scratch[1] (blur),
    //                                              binding 1 = maskColorView_.
    {
        VkDescriptorImageInfo imgInfos[2]{};
        imgInfos[0].sampler = sampler_;     imgInfos[0].imageView = glowScratchView_[1]; imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos[1].sampler = maskSampler_; imgInfos[1].imageView = maskColorView_;      imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet writes[2]{};
        for (int i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = glowCompositeDescSet_; writes[i].dstBinding = static_cast<uint32_t>(i);
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1; writes[i].pImageInfo = &imgInfos[i];
        }
        vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);
    }

    return true;
}

void VkPostProcess::runChain(VkCommandBuffer cb,
                             const std::vector<PostPass>& passes,
                             const EffectTable& effects,
                             VkExtent2D swapExtent,
                             float exposure,
                             float bloomIntensity,
                             float aoStrength) {
    for (const PostPass pass : passes) {
        switch (pass) {
            case PostPass::Copy:
                recordComposite(cb, exposure, bloomIntensity, aoStrength);
                break;

            case PostPass::Outline: {
                // Find the first active Outline style in the effect table.
                const EffectStyle* os = nullptr;
                for (int id = 1; id < EffectTable::kMaxIds; ++id) {
                    if (effects.style(static_cast<uint8_t>(id)).kind == EffectKind::Outline) {
                        os = &effects.style(static_cast<uint8_t>(id));
                        break;
                    }
                }

                OutlinePush pc{};
                if (os) {
                    pc.color[0] = os->color.x;
                    pc.color[1] = os->color.y;
                    pc.color[2] = os->color.z;
                    pc.color[3] = 1.0f;
                    pc.width    = os->width;
                } else {
                    // Fallback defaults (shouldn't happen if Outline is in passes).
                    pc.color[0] = 1.0f; pc.color[1] = 0.6f; pc.color[2] = 0.1f; pc.color[3] = 1.0f;
                    pc.width    = 2.0f;
                }
                pc.texel[0] = (swapExtent.width  > 0) ? 1.0f / static_cast<float>(swapExtent.width)  : 0.0f;
                pc.texel[1] = (swapExtent.height > 0) ? 1.0f / static_cast<float>(swapExtent.height) : 0.0f;

                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, outlinePipeline_);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        outlinePipeLayout_, 0, 1, &outlineDescSet_, 0, nullptr);
                vkCmdPushConstants(cb, outlinePipeLayout_,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(OutlinePush), &pc);
                vkCmdDraw(cb, 3, 1, 0, 0);
                break;
            }

            case PostPass::GlowComposite: {
                // GlowBlurH and GlowBlurV ran in runChainOffscreenPasses (before
                // beginViewportPass). Here we composite the blurred coverage over
                // the scene, writing into the viewport pass (which the caller has begun).
                const EffectStyle* gs = nullptr;
                for (int id = 1; id < EffectTable::kMaxIds; ++id) {
                    if (effects.style(static_cast<uint8_t>(id)).kind == EffectKind::GlowOutline) {
                        gs = &effects.style(static_cast<uint8_t>(id));
                        break;
                    }
                }

                GlowCompositePush pc{};
                if (gs) {
                    pc.color[0]  = gs->color.x;
                    pc.color[1]  = gs->color.y;
                    pc.color[2]  = gs->color.z;
                    pc.color[3]  = 1.0f;
                    pc.intensity = gs->intensity;
                } else {
                    // Fallback defaults (shouldn't happen if GlowOutline is in passes).
                    pc.color[0] = 1.0f; pc.color[1] = 0.6f; pc.color[2] = 0.1f; pc.color[3] = 1.0f;
                    pc.intensity = 1.0f;
                }

                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, glowCompositePipeline_);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        glowCompositePipeLayout_, 0, 1, &glowCompositeDescSet_, 0, nullptr);
                vkCmdPushConstants(cb, glowCompositePipeLayout_,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(GlowCompositePush), &pc);
                vkCmdDraw(cb, 3, 1, 0, 0);
                break;
            }

            case PostPass::XRay: {
                // V1 note: each in-viewport-pass draw independently samples sceneColor
                // and writes the full screen. If multiple effect kinds were active,
                // later passes would overwrite earlier ones. In practice, a single
                // effect kind is active per frame (one selected object, one kind).
                const EffectStyle* xs = nullptr;
                for (int id = 1; id < EffectTable::kMaxIds; ++id) {
                    if (effects.style(static_cast<uint8_t>(id)).kind == EffectKind::XRay) {
                        xs = &effects.style(static_cast<uint8_t>(id));
                        break;
                    }
                }

                XRayPush pc{};
                if (xs) {
                    pc.color[0]  = xs->color.x;
                    pc.color[1]  = xs->color.y;
                    pc.color[2]  = xs->color.z;
                    pc.color[3]  = 1.0f;
                    pc.intensity = xs->intensity;
                } else {
                    // Fallback defaults (shouldn't happen if XRay is in passes).
                    pc.color[0] = 1.0f; pc.color[1] = 0.6f; pc.color[2] = 0.1f; pc.color[3] = 1.0f;
                    pc.intensity = 0.5f;
                }

                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, xrayPipeline_);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        xrayPipeLayout_, 0, 1, &xrayDescSet_, 0, nullptr);
                vkCmdPushConstants(cb, xrayPipeLayout_,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(XRayPush), &pc);
                vkCmdDraw(cb, 3, 1, 0, 0);
                break;
            }

            // GlowBlurH, GlowBlurV are offscreen passes — handled in runChainOffscreenPasses.
            default:
                break;
        }
    }
}

void VkPostProcess::runChainOffscreenPasses(VkCommandBuffer cb,
                                            const std::vector<PostPass>& passes,
                                            const EffectTable& effects,
                                            VkExtent2D swapExtent) {
    // Find GlowOutline style (used for both blur passes' push constants).
    const EffectStyle* gs = nullptr;
    for (int id = 1; id < EffectTable::kMaxIds; ++id) {
        if (effects.style(static_cast<uint8_t>(id)).kind == EffectKind::GlowOutline) {
            gs = &effects.style(static_cast<uint8_t>(id));
            break;
        }
    }

    for (const PostPass pass : passes) {
        switch (pass) {
            case PostPass::GlowBlurH: {
                // Horizontal blur: read mask -> write scratch[0].
                // Runs in glowPass_ / glowFb_[0], outside the swapchain pass.
                GlowBlurPush pc{};
                pc.texel[0] = (swapExtent.width  > 0) ? 1.0f / static_cast<float>(swapExtent.width)  : 0.0f;
                pc.texel[1] = (swapExtent.height > 0) ? 1.0f / static_cast<float>(swapExtent.height) : 0.0f;
                pc.radius   = gs ? gs->width : 2.0f;
                pc._pad     = 0.0f;

                VkClearValue clearVal{};
                clearVal.color.float32[0] = 0.0f;

                VkRenderPassBeginInfo rpBegin{};
                rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                rpBegin.renderPass        = glowPass_;
                rpBegin.framebuffer       = glowFb_[0];  // H writes scratch[0]
                rpBegin.renderArea.offset = {0, 0};
                rpBegin.renderArea.extent = extent_;
                rpBegin.clearValueCount   = 1;
                rpBegin.pClearValues      = &clearVal;
                vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

                // Full-screen viewport for the scratch target.
                VkViewport vp{};
                vp.x = 0.0f; vp.y = 0.0f;
                vp.width  = static_cast<float>(extent_.width);
                vp.height = static_cast<float>(extent_.height);
                vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cb, 0, 1, &vp);
                VkRect2D scissor{{0, 0}, extent_};
                vkCmdSetScissor(cb, 0, 1, &scissor);

                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, glowBlurHPipeline_);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        glowBlurHPipeLayout_, 0, 1, &glowBlurHDescSet_, 0, nullptr);
                vkCmdPushConstants(cb, glowBlurHPipeLayout_,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(GlowBlurPush), &pc);
                vkCmdDraw(cb, 3, 1, 0, 0);
                vkCmdEndRenderPass(cb);
                break;
            }

            case PostPass::GlowBlurV: {
                // Vertical blur: read scratch[0] -> write scratch[1].
                GlowBlurPush pc{};
                pc.texel[0] = (swapExtent.width  > 0) ? 1.0f / static_cast<float>(swapExtent.width)  : 0.0f;
                pc.texel[1] = (swapExtent.height > 0) ? 1.0f / static_cast<float>(swapExtent.height) : 0.0f;
                pc.radius   = gs ? gs->width : 2.0f;
                pc._pad     = 0.0f;

                VkClearValue clearVal{};
                clearVal.color.float32[0] = 0.0f;

                VkRenderPassBeginInfo rpBegin{};
                rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                rpBegin.renderPass        = glowPass_;
                rpBegin.framebuffer       = glowFb_[1];  // V writes scratch[1]
                rpBegin.renderArea.offset = {0, 0};
                rpBegin.renderArea.extent = extent_;
                rpBegin.clearValueCount   = 1;
                rpBegin.pClearValues      = &clearVal;
                vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

                VkViewport vp{};
                vp.x = 0.0f; vp.y = 0.0f;
                vp.width  = static_cast<float>(extent_.width);
                vp.height = static_cast<float>(extent_.height);
                vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cb, 0, 1, &vp);
                VkRect2D scissor{{0, 0}, extent_};
                vkCmdSetScissor(cb, 0, 1, &scissor);

                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, glowBlurVPipeline_);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        glowBlurVPipeLayout_, 0, 1, &glowBlurVDescSet_, 0, nullptr);
                vkCmdPushConstants(cb, glowBlurVPipeLayout_,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(GlowBlurPush), &pc);
                vkCmdDraw(cb, 3, 1, 0, 0);
                vkCmdEndRenderPass(cb);
                break;
            }

            // All other passes (Copy, Outline, GlowComposite, XRay) run in runChain
            // inside the viewport pass — nothing to do here.
            default:
                break;
        }
    }
}

void VkPostProcess::runBloomOffscreenPasses(VkCommandBuffer cb, float threshold,
                                            float knee, float scatter) {
    // Degenerate extent (no mips) — nothing to record.
    if (bloomMipCount_ < 1) return;

    // Local helper: record one full-screen bloom pass into a destination mip.
    // Mirrors the glow blur recording (begin pass / dynamic viewport+scissor /
    // bind pipeline+set / push constants / draw fullscreen triangle / end pass).
    // bloomPass_ has loadOp=LOAD; downsample pipelines overwrite (blend off) so
    // the loaded content is irrelevant, upsample relies on LOAD for additive add.
    auto recordBloomPass = [&](VkFramebuffer dstFb, VkExtent2D dstExtent,
                               ::VkPipeline pipeline, VkPipelineLayout layout,
                               VkDescriptorSet srcSet, const void* pushData,
                               std::uint32_t pushSize) {
        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass        = bloomPass_;
        rpBegin.framebuffer       = dstFb;
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = dstExtent;
        rpBegin.clearValueCount   = 0;     // loadOp=LOAD — no clear
        rpBegin.pClearValues      = nullptr;
        vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{};
        vp.x = 0.0f; vp.y = 0.0f;
        vp.width    = static_cast<float>(dstExtent.width);
        vp.height   = static_cast<float>(dstExtent.height);
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        vkCmdSetViewport(cb, 0, 1, &vp);
        VkRect2D scissor{{0, 0}, dstExtent};
        vkCmdSetScissor(cb, 0, 1, &scissor);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                layout, 0, 1, &srcSet, 0, nullptr);
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, pushSize, pushData);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
    };

    // Pass 1: bright + downsample (scene -> mip0). Texel size = full scene extent.
    {
        BloomBrightPush pc{};
        pc.threshold   = threshold;
        pc.knee        = knee;
        pc.srcTexel[0] = (extent_.width  > 0) ? 1.0f / static_cast<float>(extent_.width)  : 0.0f;
        pc.srcTexel[1] = (extent_.height > 0) ? 1.0f / static_cast<float>(extent_.height) : 0.0f;
        recordBloomPass(bloomMipFbs_[0], bloomMipExtents_[0],
                        bloomBrightDownPipeline_, bloomBrightDownLayout_,
                        bloomBrightSet_, &pc, sizeof(pc));
    }

    // Pass 2: downsample chain (mip m -> mip m+1).
    for (std::uint32_t m = 0; m + 1 < bloomMipCount_; ++m) {
        BloomDownPush pc{};
        pc.srcTexel[0] = 1.0f / static_cast<float>(bloomMipExtents_[m].width);
        pc.srcTexel[1] = 1.0f / static_cast<float>(bloomMipExtents_[m].height);
        recordBloomPass(bloomMipFbs_[m + 1], bloomMipExtents_[m + 1],
                        bloomDownPipeline_, bloomDownLayout_,
                        bloomSrcSets_[m], &pc, sizeof(pc));
    }

    // Pass 3: upsample chain (mip m+1 -> mip m, additive), descending.
    for (std::uint32_t m = bloomMipCount_ - 1; m-- > 0; ) {
        // Loop runs m = bloomMipCount_-2 .. 0 (no-op if bloomMipCount_ == 1).
        BloomUpPush pc{};
        pc.srcTexel[0] = 1.0f / static_cast<float>(bloomMipExtents_[m + 1].width);
        pc.srcTexel[1] = 1.0f / static_cast<float>(bloomMipExtents_[m + 1].height);
        pc.scatter     = scatter;
        recordBloomPass(bloomMipFbs_[m], bloomMipExtents_[m],
                        bloomUpPipeline_, bloomUpLayout_,
                        bloomSrcSets_[m + 1], &pc, sizeof(pc));
    }
}

void VkPostProcess::updateSsaoUbo(int frame, const Mat4& projection, const Mat4& invProjection,
                                  float radius, float bias, float power) {
    SsaoUbo ubo{};
    ubo.projection    = projection;
    ubo.invProjection = invProjection;
    for (std::uint32_t i = 0; i < kSsaoKernelSize; ++i) {
        const Vec3& k = ssaoKernel_[i];
        ubo.kernel[i] = {k.x, k.y, k.z, 0.0f};
    }
    ubo.params     = {radius, bias, power, static_cast<float>(kSsaoKernelSize)};
    ubo.noiseScale = {ssaoExtent_.width / 4.0f, ssaoExtent_.height / 4.0f, 0.0f, 0.0f};

    std::memcpy(ssaoUboMapped_[frame], &ubo, sizeof(SsaoUbo));
    // Host-visible memory may be non-coherent — flush the per-frame write.
    vmaFlushAllocation(ctx_->allocator(), ssaoUboAlloc_[frame], 0, sizeof(SsaoUbo));
}

void VkPostProcess::runSsaoPass(VkCommandBuffer cb, int frame) {
    // Degenerate extent (targets not created) — nothing to record.
    if (ssaoExtent_.width == 0 || ssaoExtent_.height == 0) return;

    // Local helper: record one full-screen SSAO/blur pass into a framebuffer.
    // Mirrors recordBloomPass (begin pass / dynamic viewport+scissor / bind
    // pipeline+set / push constants / draw fullscreen triangle / end pass).
    // Both passes share ssaoPass_ (one render pass, two framebuffers); its
    // subpass dependencies serialize the SSAO write -> blur sample of ssaoView_.
    auto recordSsaoPass = [&](VkFramebuffer dstFb, ::VkPipeline pipeline,
                              VkPipelineLayout layout, VkDescriptorSet srcSet,
                              const void* pushData, std::uint32_t pushSize) {
        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass        = ssaoPass_;
        rpBegin.framebuffer       = dstFb;
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = ssaoExtent_;
        rpBegin.clearValueCount   = 0;     // loadOp=DONT_CARE — no clear
        rpBegin.pClearValues      = nullptr;
        vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{};
        vp.x = 0.0f; vp.y = 0.0f;
        vp.width    = static_cast<float>(ssaoExtent_.width);
        vp.height   = static_cast<float>(ssaoExtent_.height);
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        vkCmdSetViewport(cb, 0, 1, &vp);
        VkRect2D scissor{{0, 0}, ssaoExtent_};
        vkCmdSetScissor(cb, 0, 1, &scissor);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                layout, 0, 1, &srcSet, 0, nullptr);
        if (pushData) {
            vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, pushSize, pushData);
        }
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
    };

    // Pass 1: SSAO (scene depth + noise + UBO -> ssaoView_). No push constants.
    recordSsaoPass(ssaoFb_, ssaoPipeline_, ssaoLayout_, ssaoSet_[frame], nullptr, 0);

    // Pass 2: 4x4 box blur (ssaoView_ -> ssaoBlurView_). Texel = 1/extent.
    SsaoBlurPush push{
        {1.0f / static_cast<float>(ssaoExtent_.width),
         1.0f / static_cast<float>(ssaoExtent_.height)}
    };
    recordSsaoPass(ssaoBlurFb_, ssaoBlurPipeline_, ssaoBlurLayout_,
                   ssaoBlurSet_, &push, sizeof(push));
}

bool VkPostProcess::createXRayPipeline(VkContext& ctx) {
    // Compile shaders (reuse kFullscreenVert from the copy pipeline).
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT,   kFullscreenVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kXrayFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkPostProcess: x-ray shader compile failed");
        return false;
    }

    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode    = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkPostProcess: x-ray fragment shader module creation failed");
        return false;
    }

    // Descriptor set layout: 3 bindings (all COMBINED_IMAGE_SAMPLER, fragment stage).
    // Overlay — the scene comes from the Copy base, so no scene sampler.
    //   binding 0: usampler2D uMask       (mask id, NEAREST maskSampler_)
    //   binding 1: sampler2D  uMaskDepth  (tagged object depth, NEAREST maskSampler_)
    //   binding 2: sampler2D  uSceneDepth (full scene depth, NEAREST maskSampler_)
    // Depth images use NEAREST to avoid filtering across depth edges.
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (int i = 0; i < 3; ++i) {
        bindings[i].binding         = static_cast<uint32_t>(i);
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 3;
    dslInfo.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &xraySetLayout_));

    // Push constant for x-ray params (fragment stage).
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = static_cast<uint32_t>(sizeof(XRayPush));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &xraySetLayout_;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &xrayPipeLayout_));

    // Pipeline — full-screen triangle, no vertex input, depth test off.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    // Alpha-blend the x-ray tint over the composited Copy base.
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable         = VK_TRUE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.colorBlendOp        = VK_BLEND_OP_ADD;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    att.alphaBlendOp        = VK_BLEND_OP_ADD;
    att.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT;  // RGB only: preserve the
    // viewport alpha (Copy wrote a=1; the editor's ImGui::Image samples it, so
    // zeroing alpha here would make the scene render transparent).

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount          = 2;
    pInfo.pStages             = stages;
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = xrayPipeLayout_;
    pInfo.renderPass          = viewportPass_;  // composite records into viewportPass_ (must match its dependencyCount)
    pInfo.subpass             = 0;
    // Capture result so shader modules are destroyed even if pipeline creation fails.
    VkResult xrayPipeResult = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                                         &pInfo, nullptr, &xrayPipeline_);
    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    VK_CHECK(xrayPipeResult);

    // Allocate the x-ray descriptor set from the shared pool.
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool     = descPool_;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &xraySetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsAlloc, &xrayDescSet_));

    // Write the x-ray descriptor set (overlay — no scene sampler):
    //   binding 0: mask id       (NEAREST maskSampler_ — integer texture)
    //   binding 1: mask depth    (NEAREST maskSampler_ — depth, avoid edge filtering)
    //   binding 2: scene depth   (NEAREST maskSampler_ — depth, avoid edge filtering)
    VkDescriptorImageInfo imgInfos[3]{};
    imgInfos[0].sampler     = maskSampler_;
    imgInfos[0].imageView   = maskColorView_;
    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[1].sampler     = maskSampler_;
    imgInfos[1].imageView   = maskDepthView_;
    imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    imgInfos[2].sampler     = maskSampler_;
    imgInfos[2].imageView   = sceneDepthView_;
    imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = xrayDescSet_;
        writes[i].dstBinding      = static_cast<uint32_t>(i);
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &imgInfos[i];
    }
    vkUpdateDescriptorSets(ctx.device(), 3, writes, 0, nullptr);

    return true;
}

// ---------------------------------------------------------------------------
// M48 SSAO — noise texture + UBO (persistent), R8 targets + pass + sets
// (per-resize), and the 2 full-screen pipelines (persistent).
// ---------------------------------------------------------------------------

// Persistent: 4x4 RGBA16F rotation-vector noise texture (NEAREST/REPEAT),
// the host-visible per-frame SSAO UBO (persistently mapped), and the cached
// kernel. Created once in init; destroyed in destroy(). Mirrors the staging→
// image one-shot copy in VkTextureStore::uploadRgba and the mapped-buffer
// pattern (VMA_ALLOCATION_CREATE_MAPPED_BIT) in VkTexture's createStagingBuffer.
bool VkPostProcess::createSsaoNoiseAndUbo(VkContext& ctx) {
    // --- 4x4 noise texture (16 texels of (rndx,rndy,0,0), x,y in [-1,1]) ---
    constexpr std::uint32_t kNoiseDim = 4;
    constexpr std::uint32_t kNoiseTexels = kNoiseDim * kNoiseDim;
    float noise[kNoiseTexels * 4];
    {
        std::uint32_t state = 0x1234567u;  // deterministic LCG (matches Ssao.h style)
        auto rnd01 = [&state]() {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
        };
        for (std::uint32_t i = 0; i < kNoiseTexels; ++i) {
            noise[i * 4 + 0] = rnd01() * 2.0f - 1.0f;
            noise[i * 4 + 1] = rnd01() * 2.0f - 1.0f;
            noise[i * 4 + 2] = 0.0f;
            noise[i * 4 + 3] = 0.0f;
        }
    }
    // RGBA32F: upload the 16 vec4s as plain floats (vkCmdCopyBufferToImage copies
    // raw bytes, which already match R32G32B32A32_SFLOAT texel layout).
    const VkFormat noiseFmt = VK_FORMAT_R32G32B32A32_SFLOAT;
    const VkDeviceSize noiseSize = sizeof(noise);  // 16 texels * 4 floats = 256 bytes

    // Staging buffer (host-visible, mapped) — mirrors VkTexture's createStagingBuffer.
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    void* stagingMap = nullptr;
    {
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = noiseSize;
        bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo aiOut{};
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &staging, &stagingAlloc, &aiOut));
        stagingMap = aiOut.pMappedData;
    }
    std::memcpy(stagingMap, noise, noiseSize);
    vmaFlushAllocation(ctx.allocator(), stagingAlloc, 0, noiseSize);

    // Noise image (SAMPLED | TRANSFER_DST).
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = noiseFmt;
        iInfo.extent        = {kNoiseDim, kNoiseDim, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aInfo{};
        aInfo.usage         = VMA_MEMORY_USAGE_AUTO;
        aInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo, &ssaoNoise_, &ssaoNoiseAlloc_, nullptr));
    }

    // One-shot copy (UNDEFINED → TRANSFER_DST → copy → SHADER_READ_ONLY).
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pInfo{};
        pInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pInfo.queueFamilyIndex = ctx.graphicsFamily();
        pInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VK_CHECK(vkCreateCommandPool(ctx.device(), &pInfo, nullptr, &pool));

        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbInfo{};
        cbInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbInfo.commandPool        = pool;
        cbInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb));

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &begin));

        VkImageMemoryBarrier toDst{};
        toDst.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.image            = ssaoNoise_;
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toDst.srcAccessMask    = 0;
        toDst.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {kNoiseDim, kNoiseDim, 1};
        vkCmdCopyBufferToImage(cb, staging, ssaoNoise_,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier toShader = toDst;
        toShader.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);

        VK_CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cb;
        VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));
        vkDestroyCommandPool(ctx.device(), pool, nullptr);
    }
    vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);

    // Noise view.
    {
        VkImageViewCreateInfo vInfo{};
        vInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image            = ssaoNoise_;
        vInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format           = noiseFmt;
        vInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &ssaoNoiseView_));
    }

    // Noise sampler: NEAREST + REPEAT (the 4x4 noise tiles across the screen).
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_NEAREST;
        si.minFilter    = VK_FILTER_NEAREST;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VK_CHECK(vkCreateSampler(ctx.device(), &si, nullptr, &ssaoNoiseSampler_));
    }

    // --- Persistent host-visible, persistently-mapped SSAO UBO (one per
    // frame-in-flight slot so the CPU write for this frame can't race the
    // previous frame's GPU read). ---
    for (int f = 0; f < 2; ++f) {
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = sizeof(SsaoUbo);
        bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo aiOut{};
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &ssaoUboBuf_[f], &ssaoUboAlloc_[f], &aiOut));
        ssaoUboMapped_[f] = aiOut.pMappedData;
    }

    // Cache the kernel (generated once; uploaded per-frame in Task 4).
    ssaoKernel_ = generateSsaoKernel(kSsaoKernelSize);
    return true;
}

// Per-resize: R8 AO + blurred-AO images/views/fbs, the persistent single-R8
// render pass (create-if-null, like bloomPass_), and the 2 descriptor sets
// (re-allocated each resize because they reference the rebuilt ssaoView_ +
// sceneDepthView_). Mirrors createBloomTargets' structure.
void VkPostProcess::createSsaoTargets(VkContext& ctx, VkExtent2D extent) {
    ssaoExtent_ = extent;

    auto makeR8 = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view) {
        VkImageCreateInfo iInfo{};
        iInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType     = VK_IMAGE_TYPE_2D;
        iInfo.format        = VK_FORMAT_R8_UNORM;
        iInfo.extent        = {extent.width, extent.height, 1};
        iInfo.mipLevels     = 1;
        iInfo.arrayLayers   = 1;
        iInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo, &img, &alloc, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image            = img;
        vInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format           = VK_FORMAT_R8_UNORM;
        vInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &view));
    };
    makeR8(ssaoImage_,     ssaoAlloc_,     ssaoView_);
    makeR8(ssaoBlurImage_, ssaoBlurAlloc_, ssaoBlurView_);

    // Single R8 color attachment pass. loadOp=DONT_CARE (both passes fully
    // overwrite, no blend), finalLayout SHADER_READ_ONLY so the result is
    // immediately sampleable. No COLOR_ATTACHMENT_READ_BIT (no LOAD/blend).
    if (ssaoPass_ == VK_NULL_HANDLE) {  // persist across resize (see bloomPass_)
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_R8_UNORM;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments    = &att;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 2;
        rpInfo.pDependencies   = deps;
        VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &ssaoPass_));
    }

    // Framebuffers (one per R8 target), both at full extent.
    auto makeFb = [&](VkImageView view, VkFramebuffer& fb) {
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = ssaoPass_;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &view;
        fbInfo.width           = extent.width;
        fbInfo.height          = extent.height;
        fbInfo.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &fb));
    };
    makeFb(ssaoView_,     ssaoFb_);
    makeFb(ssaoBlurView_, ssaoBlurFb_);

    // Allocate + write the 3 descriptor sets from the persistent ssaoDescPool_
    // (2 per-frame ssao sets + 1 blur set). Reset the pool first so a resize
    // re-allocates cleanly (the pool is sized for exactly these 3 sets). The set
    // layouts + pool were created in init before createTargets ran — heeds the
    // M47 null-layout lesson.
    VK_CHECK(vkResetDescriptorPool(ctx.device(), ssaoDescPool_, 0));
    for (int f = 0; f < 2; ++f) {
        VkDescriptorSetAllocateInfo a0{};
        a0.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        a0.descriptorPool     = ssaoDescPool_;
        a0.descriptorSetCount = 1;
        a0.pSetLayouts        = &ssaoSetLayout_;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &a0, &ssaoSet_[f]));
    }
    {
        VkDescriptorSetAllocateInfo a1{};
        a1.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        a1.descriptorPool     = ssaoDescPool_;
        a1.descriptorSetCount = 1;
        a1.pSetLayouts        = &ssaoBlurSetLayout_;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &a1, &ssaoBlurSet_));
    }

    // ssaoSet_[f]: 0 = scene depth (maskSampler_ NEAREST + depth layout, like
    // x-ray), 1 = noise (NEAREST/REPEAT), 2 = frame f's UBO (whole range).
    for (int f = 0; f < 2; ++f) {
        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler     = maskSampler_;
        depthInfo.imageView   = sceneDepthView_;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo noiseInfo{};
        noiseInfo.sampler     = ssaoNoiseSampler_;
        noiseInfo.imageView   = ssaoNoiseView_;
        noiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = ssaoUboBuf_[f];
        uboInfo.offset = 0;
        uboInfo.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet w[3]{};
        w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet          = ssaoSet_[f];
        w[0].dstBinding      = 0;
        w[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].descriptorCount = 1;
        w[0].pImageInfo      = &depthInfo;
        w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet          = ssaoSet_[f];
        w[1].dstBinding      = 1;
        w[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].descriptorCount = 1;
        w[1].pImageInfo      = &noiseInfo;
        w[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[2].dstSet          = ssaoSet_[f];
        w[2].dstBinding      = 2;
        w[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[2].descriptorCount = 1;
        w[2].pBufferInfo     = &uboInfo;
        vkUpdateDescriptorSets(ctx.device(), 3, w, 0, nullptr);
    }

    // ssaoBlurSet_: 0 = raw AO (ssaoView_) via the shared linear-clamp sampler
    // (bloomSampler_ is LINEAR + CLAMP_TO_EDGE — exactly what the box blur taps want).
    {
        VkDescriptorImageInfo aoInfo{};
        aoInfo.sampler     = bloomSampler_;
        aoInfo.imageView   = ssaoView_;
        aoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = ssaoBlurSet_;
        w.dstBinding      = 0;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo      = &aoInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &w, 0, nullptr);
    }
}

void VkPostProcess::destroySsaoTargets(VkContext& ctx) {
    // Per-resize handles only. ssaoPass_ persists (freed in destroy()).
    // The descriptor sets live in the persistent ssaoDescPool_; resetting it on
    // the next createSsaoTargets frees them, and destroy() frees the pool itself.
    if (ssaoDescPool_) {
        vkResetDescriptorPool(ctx.device(), ssaoDescPool_, 0);
        ssaoSet_[0]  = VK_NULL_HANDLE;
        ssaoSet_[1]  = VK_NULL_HANDLE;
        ssaoBlurSet_ = VK_NULL_HANDLE;
    }
    if (ssaoBlurFb_)    { vkDestroyFramebuffer(ctx.device(), ssaoBlurFb_, nullptr); ssaoBlurFb_ = VK_NULL_HANDLE; }
    if (ssaoFb_)        { vkDestroyFramebuffer(ctx.device(), ssaoFb_, nullptr); ssaoFb_ = VK_NULL_HANDLE; }
    if (ssaoBlurView_)  { vkDestroyImageView(ctx.device(), ssaoBlurView_, nullptr); ssaoBlurView_ = VK_NULL_HANDLE; }
    if (ssaoView_)      { vkDestroyImageView(ctx.device(), ssaoView_, nullptr); ssaoView_ = VK_NULL_HANDLE; }
    if (ssaoBlurImage_) { vmaDestroyImage(ctx.allocator(), ssaoBlurImage_, ssaoBlurAlloc_); ssaoBlurImage_ = VK_NULL_HANDLE; ssaoBlurAlloc_ = VK_NULL_HANDLE; }
    if (ssaoImage_)     { vmaDestroyImage(ctx.allocator(), ssaoImage_, ssaoAlloc_); ssaoImage_ = VK_NULL_HANDLE; ssaoAlloc_ = VK_NULL_HANDLE; }
}

// Persistent: the SSAO + blur full-screen pipelines, built against ssaoPass_
// (created by createSsaoTargets before this runs in init). Mirrors the bloom
// fullscreen pipeline builder. Returns false on shader-compile failure.
bool VkPostProcess::createSsaoPipelines(VkContext& ctx) {
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kFullscreenVert);
    if (vspv.empty()) {
        Log::error("VkPostProcess: SSAO vertex shader compile failed");
        return false;
    }
    auto makeModule = [&](const std::vector<std::uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo smInfo{};
        smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smInfo.codeSize = spv.size() * sizeof(std::uint32_t);
        smInfo.pCode    = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        return m;
    };

    // Pipeline layouts: SSAO = ssaoSetLayout_, no push; blur = ssaoBlurSetLayout_ +
    // FRAGMENT push (SsaoBlurPush).
    {
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount = 1;
        plInfo.pSetLayouts    = &ssaoSetLayout_;
        VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &ssaoLayout_));
    }
    {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.offset     = 0;
        pc.size       = sizeof(SsaoBlurPush);
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &ssaoBlurSetLayout_;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &pc;
        VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &ssaoBlurLayout_));
    }

    // Common full-screen fixed-function state (mirrors createBloomPipelines).
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1;
    vpState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsState{};
    dsState.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dsState.depthTestEnable  = VK_FALSE;
    dsState.depthWriteEnable = VK_FALSE;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable    = VK_FALSE;   // both passes fully overwrite — no blend
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &att;

    auto buildPipeline = [&](const char* frag, VkPipelineLayout layout,
                             ::VkPipeline* outPipe, const char* tag) -> bool {
        auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, frag);
        if (fspv.empty()) { Log::error("VkPostProcess: SSAO %s shader compile failed", tag); return false; }
        VkShaderModule vsm = makeModule(vspv);
        VkShaderModule fsm = makeModule(fspv);
        if (!vsm || !fsm) {
            if (vsm) vkDestroyShaderModule(ctx.device(), vsm, nullptr);
            if (fsm) vkDestroyShaderModule(ctx.device(), fsm, nullptr);
            Log::error("VkPostProcess: SSAO %s shader module creation failed", tag);
            return false;
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vsm; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fsm; stages[1].pName = "main";

        VkGraphicsPipelineCreateInfo pInfo{};
        pInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pInfo.stageCount          = 2; pInfo.pStages = stages;
        pInfo.pVertexInputState   = &vi;
        pInfo.pInputAssemblyState = &ia;
        pInfo.pViewportState      = &vpState;
        pInfo.pRasterizationState = &rs;
        pInfo.pMultisampleState   = &ms;
        pInfo.pDepthStencilState  = &dsState;
        pInfo.pColorBlendState    = &blend;
        pInfo.pDynamicState       = &dyn;
        pInfo.layout              = layout;
        pInfo.renderPass          = ssaoPass_;
        pInfo.subpass             = 0;
        VkResult r = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, outPipe);
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        vkDestroyShaderModule(ctx.device(), fsm, nullptr);
        VK_CHECK(r);
        return true;
    };

    if (!buildPipeline(kSsaoSrc(),     ssaoLayout_,     &ssaoPipeline_,     "occlusion")) return false;
    if (!buildPipeline(kSsaoBlurSrc(), ssaoBlurLayout_, &ssaoBlurPipeline_, "blur"))      return false;
    return true;
}

}  // namespace iron
