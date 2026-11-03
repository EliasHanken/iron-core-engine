#include "render/backends/vulkan/VkDdgi.h"

#include "core/Log.h"
#include "math/Quaternion.h"
#include "render/GiShaderCommon.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace iron {

namespace {

// The parameter block, std140, in the order of giParamsMembersGlsl().
struct GpuGiParams {
    float        origin[4];
    float        spacing[4];
    std::int32_t counts[4];
    float        atlas[4];
    float        rotation[16];
    float        sun[4];
    float        sunColor[4];
    float        misc[4];
    float        boundsMin[4];
    float        boundsMax[4];
};
static_assert(sizeof(GpuGiParams) == 208, "std140 GiParams");

constexpr std::uint32_t kIrradianceTile = 8;  // 6 inside a border.
constexpr std::uint32_t kDistanceTile = 16;   // 14 inside a border.
constexpr std::uint32_t kMaxAtlasSide = 16384;
constexpr std::uint32_t kMaxProbes = 16384;

std::string paramsBlock(int binding) {
    return "layout(std140, set = 0, binding = " + std::to_string(binding) + ") uniform GiParams {" +
           std::string(giParamsMembersGlsl()) + "} gi;\n";
}
std::string offsetsBlock(int binding, bool writable) {
    return "layout(std430, set = 0, binding = " + std::to_string(binding) + ") " + (writable ? "" : "readonly ") +
           "buffer GiOffsets { vec4 giOffsets[]; };\n";
}

// The probes' rays: each lit at what it hits by the sun (if the sun can see it),
// the lights (that can), and the probes' light there from last frame (which is
// how light bounces again and again), or the sky if it hits nothing.
std::string traceGlsl() {
    return std::string(R"GLSL(#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = 32) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;
)GLSL") + paramsBlock(1) + R"GLSL(
struct Info { uvec2 vertices; uvec2 indices; vec4 albedo; vec4 emissive; };
layout(std430, set = 0, binding = 2) readonly buffer Infos { Info infos[]; };
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D rays;
layout(set = 0, binding = 4) uniform sampler2D irradianceAtlas;
layout(set = 0, binding = 5) uniform sampler2D distanceAtlas;
layout(set = 0, binding = 6) uniform samplerCube sky;
struct Light { vec4 positionRange; vec4 colorIntensity; vec4 spot; vec4 spot2; };
layout(std430, set = 0, binding = 7) readonly buffer Lights { Light lights[]; };
layout(std430, set = 0, binding = 8) readonly buffer GiOffsets { vec4 giOffsets[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Floats { float f[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Uints { uint u[]; };
)GLSL" + giCommonGlsl() + R"GLSL(
vec3 sphericalFibonacci(float i, float n) {
    const float PHI = 1.6180339887;
    float phi = 6.2831853 * fract(i * (PHI - 1.0));
    float cosTheta = 1.0 - (2.0 * i + 1.0) / n;
    float sinTheta = sqrt(clamp(1.0 - cosTheta * cosTheta, 0.0, 1.0));
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}
bool occluded(vec3 o, vec3 d, float tmax) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, o, 0.0, d, tmax);
    while (rayQueryProceedEXT(q)) {}
    return rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}
void main() {
    int ray = int(gl_GlobalInvocationID.x);
    int probe = int(gl_GlobalInvocationID.y);
    if (ray >= gi.counts.w) return;
    vec3 origin = giProbePos(giProbeCoord(probe));
    vec3 dir = normalize(mat3(gi.rotation) * sphericalFibonacci(float(ray), float(gi.counts.w)));
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, gl_RayFlagsOpaqueEXT, 0xFF, origin, 0.0, dir, gi.misc.x);
    while (rayQueryProceedEXT(q)) {}
    vec4 result;
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        result = vec4(textureLod(sky, dir, 0.0).rgb, 1e4);
    } else {
        float t = rayQueryGetIntersectionTEXT(q, true);
        Info info = infos[rayQueryGetIntersectionInstanceCustomIndexEXT(q, true)];
        int prim = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
        vec2 bary = rayQueryGetIntersectionBarycentricsEXT(q, true);
        mat4x3 toWorld = rayQueryGetIntersectionObjectToWorldEXT(q, true);
        Uints idx = Uints(info.indices);
        Floats v = Floats(info.vertices);
        uint i0 = idx.u[prim * 3], i1 = idx.u[prim * 3 + 1], i2 = idx.u[prim * 3 + 2];
        // The vertex is position (3), normal (3), uv (2), tangent (3).
        vec3 n0 = vec3(v.f[i0 * 11 + 3], v.f[i0 * 11 + 4], v.f[i0 * 11 + 5]);
        vec3 n1 = vec3(v.f[i1 * 11 + 3], v.f[i1 * 11 + 4], v.f[i1 * 11 + 5]);
        vec3 n2 = vec3(v.f[i2 * 11 + 3], v.f[i2 * 11 + 4], v.f[i2 * 11 + 5]);
        vec3 N = normalize(mat3(toWorld) * (n0 * (1.0 - bary.x - bary.y) + n1 * bary.x + n2 * bary.y));
        bool back = dot(N, dir) > 0.0;
        if (back && !rayQueryGetIntersectionFrontFaceEXT(q, true)) {
            // Inside something: no light here, and the probe should know it's buried.
            result = vec4(0.0, 0.0, 0.0, -t * 0.2);
        } else {
            if (back) N = -N;
            vec3 P = origin + dir * t + N * 0.02;
            vec3 direct = vec3(0.0);
            float sunN = max(dot(N, gi.sun.xyz), 0.0);
            if (sunN > 0.0 && !occluded(P, gi.sun.xyz, 2000.0)) direct += gi.sunColor.rgb * sunN;
            int count = int(gi.sunColor.w);
            for (int k = 0; k < count; ++k) {
                Light l = lights[k];
                vec3 toLight = l.positionRange.xyz - P;
                float dist = length(toLight);
                float range = l.positionRange.w;
                if (dist >= range || dist < 1e-4) continue;
                vec3 L = toLight / dist;
                float nl = max(dot(N, L), 0.0);
                if (nl <= 0.0) continue;
                float falloff = 1.0 - smoothstep(0.0, range, dist);
                if (l.spot.w > -1.5) falloff *= smoothstep(l.spot.w, max(l.spot2.x, l.spot.w + 1e-4), dot(-L, l.spot.xyz));
                vec3 radiance = l.colorIntensity.rgb * l.colorIntensity.w * falloff;
                if (dot(radiance, vec3(0.333)) * nl < 0.002) continue;
                if (occluded(P, L, dist - 0.05)) continue;
                direct += radiance * nl;
            }
            vec3 bounced = giIrradiance(irradianceAtlas, distanceAtlas, P, N, -dir) * gi.misc.z;
            result = vec4(info.albedo.rgb * (direct / 3.14159265 + bounced) + info.emissive.rgb, t);
        }
    }
    imageStore(rays, ivec2(ray, probe), result);
}
)GLSL";
}

// Each probe's texels take this frame's rays (cosine-weighted for light, sharply
// for distance), blended into what they held.
std::string blendGlsl(bool distance) {
    const int texels = distance ? 14 : 6;
    std::string s = "#version 460\nlayout(local_size_x = " + std::to_string(texels) +
                    ", local_size_y = " + std::to_string(texels) + ") in;\n" + paramsBlock(0) +
                    "layout(set = 0, binding = 1, rgba16f) uniform readonly image2D rays;\n"
                    "layout(set = 0, binding = 2, rgba16f) uniform image2D atlas;\n" + offsetsBlock(3, false) +
                    giCommonGlsl() + R"GLSL(
vec3 sphericalFibonacci(float i, float n) {
    const float PHI = 1.6180339887;
    float phi = 6.2831853 * fract(i * (PHI - 1.0));
    float cosTheta = 1.0 - (2.0 * i + 1.0) / n;
    float sinTheta = sqrt(clamp(1.0 - cosTheta * cosTheta, 0.0, 1.0));
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}
void main() {
    int probe = int(gl_WorkGroupID.x);
    ivec2 local = ivec2(gl_LocalInvocationID.xy);
)GLSL";
    s += "    const int TEXELS = " + std::to_string(texels) + ";\n";
    s += distance ? "    const bool DISTANCE = true;\n" : "    const bool DISTANCE = false;\n";
    s += R"GLSL(
    vec3 texelDir = giOctDecode(((vec2(local) + 0.5) / float(TEXELS)) * 2.0 - 1.0);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int r = 0; r < gi.counts.w; ++r) {
        vec4 hit = imageLoad(rays, ivec2(r, probe));
        vec3 dir = normalize(mat3(gi.rotation) * sphericalFibonacci(float(r), float(gi.counts.w)));
        float w = max(dot(texelDir, dir), 0.0);
        if (DISTANCE) {
            w = pow(w, 50.0);
            float d = min(abs(hit.a), gi.misc.x);
            sum += w * vec3(d, d * d, 0.0);
        } else {
            if (hit.a < 0.0) continue;
            sum += w * hit.rgb;
        }
        total += w;
    }
    ivec2 px = giTile(giProbeCoord(probe)) * (TEXELS + 2) + 1 + local;
    vec3 fresh = sum / max(total, 1e-4);
    vec4 old = imageLoad(atlas, px);
    float keep;
    if (DISTANCE) {
        keep = gi.misc.w < 0.5 ? 0.0 : min(gi.sun.w, 0.95);
    } else {
        // Light is blended as irradiance^(1/5), as RTXGI does: in that space a
        // single bright ray can't spike a dark probe, so the noise doesn't flicker.
        // Small changes are noise: blend them slowly. Big ones are the light
        // really changing (a lamp on, the sun moving): let them through quickly.
        // Judged in linear light, relative to how bright it is.
        vec3 oldLinear = old.rgb * old.rgb;
        oldLinear = oldLinear * oldLinear * old.rgb;
        const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
        float a = dot(fresh, lum), b = dot(oldLinear, lum);
        float change = abs(a - b) / max(max(a, b), 0.02);
        float settled = clamp(gi.sun.w + 0.015, 0.0, 0.995);
        keep = mix(settled, 0.85, smoothstep(0.7, 1.0, change));
        fresh = pow(max(fresh, vec3(0.0)), vec3(0.2));
        if (gi.misc.w < 0.5) keep = 0.0;
    }
    imageStore(atlas, px, vec4(mix(fresh, old.rgb, keep), 1.0));
}
)GLSL";
    return s;
}

// Where a probe should be: pushed out of whatever it's buried in (most of its
// rays see backfaces), or away from a surface it's pressed against, but never
// more than 45% of a spacing from its grid point.
std::string relocateGlsl() {
    return "#version 460\nlayout(local_size_x = 32) in;\n" + paramsBlock(0) +
           "layout(set = 0, binding = 1, rgba16f) uniform readonly image2D rays;\n" + offsetsBlock(2, true) +
           giCommonGlsl() + R"GLSL(
vec3 sphericalFibonacci(float i, float n) {
    const float PHI = 1.6180339887;
    float phi = 6.2831853 * fract(i * (PHI - 1.0));
    float cosTheta = 1.0 - (2.0 * i + 1.0) / n;
    float sinTheta = sqrt(clamp(1.0 - cosTheta * cosTheta, 0.0, 1.0));
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}
void main() {
    int probe = int(gl_GlobalInvocationID.x);
    if (probe >= gi.counts.x * gi.counts.y * gi.counts.z) return;
    float spacing = min(gi.spacing.x, min(gi.spacing.y, gi.spacing.z));
    float minFront = 0.3 * spacing;
    int backfaces = 0;
    float closestBack = 1e9, closestFront = 1e9, farthestFront = 0.0;
    vec3 backDir = vec3(0.0), frontDir = vec3(0.0), farDir = vec3(0.0);
    for (int r = 0; r < gi.counts.w; ++r) {
        float d = imageLoad(rays, ivec2(r, probe)).a;
        vec3 dir = normalize(mat3(gi.rotation) * sphericalFibonacci(float(r), float(gi.counts.w)));
        if (d < 0.0) {
            ++backfaces;
            float t = -d * 5.0;  // Backface distances are stored at a fifth.
            if (t < closestBack) { closestBack = t; backDir = dir; }
        } else {
            if (d < closestFront) { closestFront = d; frontDir = dir; }
            if (d > farthestFront) { farthestFront = d; farDir = dir; }
        }
    }
    vec3 offset = giOffsets[probe].xyz;
    if (float(backfaces) / float(gi.counts.w) > 0.25) {
        // Inside something: through its nearest face, and a little beyond.
        offset += backDir * (closestBack + minFront * 0.5);
    } else if (closestFront < minFront) {
        // Against something: off it, toward open space.
        offset += normalize(farDir - frontDir * 0.5) * min(minFront - closestFront, farthestFront * 0.5);
    }
    offset = clamp(offset, vec3(-0.45 * spacing), vec3(0.45 * spacing));
    giOffsets[probe] = vec4(offset, 0.0);
}
)GLSL";
}

// Each tile's one-texel border copied from the opposite side of its inside, so
// bilinear filtering wraps over the octahedron's edges the way directions do.
const char* const kBorderGlsl = R"GLSL(#version 460
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0, rgba16f) uniform image2D atlas;
layout(push_constant) uniform Push { int texels; int tilesX; } pc;
void main() {
    int full = pc.texels + 2;
    ivec2 l = ivec2(gl_LocalInvocationID.xy);
    if (l.x >= full || l.y >= full) return;
    bool onX = l.x == 0 || l.x == full - 1;
    bool onY = l.y == 0 || l.y == full - 1;
    if (!onX && !onY) return;
    int tile = int(gl_WorkGroupID.x);
    ivec2 origin = ivec2(tile % pc.tilesX, tile / pc.tilesX) * full;
    ivec2 src;
    if (onX && onY) src = ivec2(l.x == 0 ? full - 2 : 1, l.y == 0 ? full - 2 : 1);
    else if (onY) src = ivec2(full - 1 - l.x, l.y == 0 ? 1 : full - 2);
    else src = ivec2(l.x == 0 ? 1 : full - 2, full - 1 - l.y);
    imageStore(atlas, origin + l, imageLoad(atlas, origin + src));
}
)GLSL";

void computeBarrier(VkCommandBuffer cb, VkPipelineStageFlags dst) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

}  // namespace

VkDeviceSize VkDdgi::paramsBytes() const { return sizeof(GpuGiParams); }

bool VkDdgi::init(VkContext& ctx) {
    for (Buffer& b : params_) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = sizeof(GpuGiParams);
        info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo out{};
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &alloc, &b.buffer, &b.alloc, &out));
        b.mapped = out.pMappedData;
        GpuGiParams none{};
        std::memcpy(b.mapped, &none, sizeof(none));
    }
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = VK_FILTER_LINEAR;
    s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(ctx.device(), &s, nullptr, &sampler_));
    return makeImages(ctx, 1, 1, 1, 32);
}

void VkDdgi::destroy(VkContext& ctx) {
    freeImages(ctx);
    for (Buffer& b : params_) {
        if (b.buffer) vmaDestroyBuffer(ctx.allocator(), b.buffer, b.alloc);
        b = {};
    }
    if (sampler_) vkDestroySampler(ctx.device(), sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    trace_.destroy(ctx);
    blendIrradiance_.destroy(ctx);
    blendDistance_.destroy(ctx);
    border_.destroy(ctx);
    relocate_.destroy(ctx);
}

void VkDdgi::freeImages(VkContext& ctx) {
    if (offsets_.buffer) vmaDestroyBuffer(ctx.allocator(), offsets_.buffer, offsets_.alloc);
    offsets_ = {};
    rayData_.destroy(ctx);
    irradiance_.destroy(ctx);
    distance_.destroy(ctx);
}

bool VkDdgi::makeImages(VkContext& ctx, std::uint32_t px, std::uint32_t py, std::uint32_t pz, std::uint32_t rays) {
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const std::uint32_t probes = px * py * pz;
    if (!rayData_.create2D(ctx, rays, probes, VK_FORMAT_R16G16B16A16_SFLOAT, usage) ||
        !irradiance_.create2D(ctx, px * py * kIrradianceTile, pz * kIrradianceTile, VK_FORMAT_R16G16B16A16_SFLOAT, usage) ||
        !distance_.create2D(ctx, px * py * kDistanceTile, pz * kDistanceTile, VK_FORMAT_R16G16B16A16_SFLOAT, usage)) {
        return false;
    }
    counts_[0] = px;
    counts_[1] = py;
    counts_[2] = pz;
    rays_ = rays;
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = sizeof(float) * 4 * probes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &alloc, &offsets_.buffer, &offsets_.alloc, nullptr));
    }
    // Into GENERAL (where they stay: written by compute, read by every lit shader), cleared.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = ctx.graphicsFamily();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &poolInfo, nullptr, &pool));
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbInfo.commandPool = pool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));
    for (VkVolumeImage* image : {&rayData_, &irradiance_, &distance_}) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = image->image();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, image->image(), VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    vkCmdFillBuffer(cb, offsets_.buffer, 0, VK_WHOLE_SIZE, 0);
    VkMemoryBarrier done{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    done.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    done.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &done, 0,
                         nullptr, 0, nullptr);
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
    vkQueueWaitIdle(ctx.graphicsQueue());
    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    framesSinceReset_ = 0;
    ++generation_;
    return true;
}

void VkDdgi::setVolume(VkContext& ctx, const GiVolume& volume) {
    if (!ctx.rayTracing()) {
        Log::warn("VkDdgi: no hardware ray tracing; the GI volume is ignored");
        return;
    }
    GiVolume v = volume;
    v.raysPerProbe = std::clamp((v.raysPerProbe + 31) / 32 * 32, 32, 256);
    // As many probes as fit the atlases and the ray buffer, spacing them out if need be.
    std::uint32_t c[3];
    for (;;) {
        for (int i = 0; i < 3; ++i) {
            const float extent = std::max((&v.max.x)[i] - (&v.min.x)[i], 0.0f);
            c[i] = std::max(static_cast<std::uint32_t>(std::floor(extent / v.spacing)) + 1, 2u);
        }
        if (c[0] * c[1] * kDistanceTile <= kMaxAtlasSide && c[2] * kDistanceTile <= kMaxAtlasSide &&
            c[0] * c[1] * c[2] <= kMaxProbes) {
            break;
        }
        v.spacing *= 1.15f;
    }
    if (v.spacing != volume.spacing) Log::warn("VkDdgi: probe spacing widened to %.2f m to fit", v.spacing);
    vkDeviceWaitIdle(ctx.device());
    freeImages(ctx);
    if (!makeImages(ctx, c[0], c[1], c[2], static_cast<std::uint32_t>(v.raysPerProbe))) {
        Log::error("VkDdgi: couldn't make the probe atlases");
        live_ = false;
        return;
    }
    volume_ = v;
    live_ = true;
    Log::info("VkDdgi: %u x %u x %u probes (%u), %.2f m apart, %d rays each", c[0], c[1], c[2], c[0] * c[1] * c[2],
              v.spacing, v.raysPerProbe);
}

void VkDdgi::clearVolume(VkContext& ctx) {
    if (!live_) return;
    vkDeviceWaitIdle(ctx.device());
    freeImages(ctx);
    makeImages(ctx, 1, 1, 1, 32);
    live_ = false;
}

void VkDdgi::update(VulkanRenderer& renderer, VkCommandBuffer cb, std::uint32_t frame, VkAccelerationStructureKHR scene,
                    VkBuffer instanceInfo, const Lighting& lighting) {
    VkContext& ctx = renderer.context();
    const bool tracing = live_ && scene != VK_NULL_HANDLE && instanceInfo != VK_NULL_HANDLE && lighting.sky &&
                         lighting.lights;
    // This frame's parameters.
    GpuGiParams p{};
    const float spacing = volume_.spacing;
    p.origin[0] = volume_.min.x;
    p.origin[1] = volume_.min.y;
    p.origin[2] = volume_.min.z;
    p.origin[3] = live_ && framesSinceReset_ > 0 ? 1.0f : 0.0f;  // Lit surfaces wait for one pass.
    p.spacing[0] = p.spacing[1] = p.spacing[2] = spacing;
    p.spacing[3] = 0.45f;
    for (int i = 0; i < 3; ++i) p.counts[i] = static_cast<std::int32_t>(counts_[i]);
    p.counts[3] = static_cast<std::int32_t>(rays_);
    p.atlas[0] = static_cast<float>(counts_[0] * counts_[1] * kIrradianceTile);
    p.atlas[1] = static_cast<float>(counts_[2] * kIrradianceTile);
    p.atlas[2] = static_cast<float>(counts_[0] * counts_[1] * kDistanceTile);
    p.atlas[3] = static_cast<float>(counts_[2] * kDistanceTile);
    // A fresh random turn of the ray set each frame, so the probes see every direction over time.
    ++frameNumber_;
    std::uint32_t seed = frameNumber_ * 747796405u + 2891336453u;
    const auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) * (1.0f / 16777216.0f);
    };
    const float z = next() * 2.0f - 1.0f, a = next() * 6.2831853f;
    const float r = std::sqrt(std::max(1.0f - z * z, 0.0f));
    const Mat4 turn = Quat::fromAxisAngle(Vec3{r * std::cos(a), r * std::sin(a), z}, next() * 6.2831853f).toMat4();
    std::memcpy(p.rotation, turn.m, sizeof(p.rotation));
    const Vec3 towardSun = normalize(lighting.towardSun);
    p.sun[0] = towardSun.x;
    p.sun[1] = towardSun.y;
    p.sun[2] = towardSun.z;
    p.sun[3] = volume_.hysteresis;
    p.sunColor[0] = lighting.sunColor.x;
    p.sunColor[1] = lighting.sunColor.y;
    p.sunColor[2] = lighting.sunColor.z;
    p.sunColor[3] = static_cast<float>(std::min<std::uint32_t>(lighting.lightCount, 64));
    p.misc[0] = volume_.maxRayDistance;
    p.misc[1] = static_cast<float>(static_cast<int>(view_));
    p.misc[2] = volume_.energy;
    p.misc[3] = static_cast<float>(framesSinceReset_);
    p.boundsMin[0] = volume_.min.x;
    p.boundsMin[1] = volume_.min.y;
    p.boundsMin[2] = volume_.min.z;
    p.boundsMin[3] = wipe_;
    p.boundsMax[0] = volume_.min.x + spacing * static_cast<float>(counts_[0] - 1);
    p.boundsMax[1] = volume_.min.y + spacing * static_cast<float>(counts_[1] - 1);
    p.boundsMax[2] = volume_.min.z + spacing * static_cast<float>(counts_[2] - 1);
    if (!tracing) p.origin[3] = 0.0f;
    std::memcpy(params_[frame].mapped, &p, sizeof(p));
    vmaFlushAllocation(ctx.allocator(), params_[frame].alloc, 0, VK_WHOLE_SIZE);
    if (!tracing) return;

    if (!stagesReady_) {
        const bool ok =
            trace_.init(ctx, traceGlsl(),
                        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}) &&
            blendIrradiance_.init(ctx, blendGlsl(false),
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}) &&
            blendDistance_.init(ctx, blendGlsl(true),
                                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}) &&
            relocate_.init(ctx, relocateGlsl(),
                           {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}) &&
            border_.init(ctx, kBorderGlsl, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, 8);
        if (!ok) {
            Log::error("VkDdgi: the probe shaders didn't build; GI is off");
            live_ = false;
            return;
        }
        stagesReady_ = true;
    }
    const std::uint32_t probes = counts_[0] * counts_[1] * counts_[2];
    const VkComputeResource paramsRes = VkComputeResource::bufferRange(params_[frame].buffer, 0, sizeof(GpuGiParams));
    // 1. The rays.
    trace_.dispatch(renderer, cb,
                    {VkComputeResource::accelerationStructure(scene), paramsRes,
                     VkComputeResource::bufferRange(instanceInfo), VkComputeResource::image(rayData_.view()),
                     VkComputeResource::sampled(irradiance_.view(), sampler_, VK_IMAGE_LAYOUT_GENERAL),
                     VkComputeResource::sampled(distance_.view(), sampler_, VK_IMAGE_LAYOUT_GENERAL),
                     VkComputeResource::sampled(lighting.sky, lighting.skySampler),
                     VkComputeResource::bufferRange(lighting.lights), VkComputeResource::bufferRange(offsets_.buffer)},
                    nullptr, rays_ / 32, probes);
    computeBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    // 1b. Probes out of walls, for the next frame's rays (the first few hundred frames).
    if (framesSinceReset_ < 600) {
        relocate_.dispatch(renderer, cb,
                           {paramsRes, VkComputeResource::image(rayData_.view()),
                            VkComputeResource::bufferRange(offsets_.buffer)},
                           nullptr, (probes + 31) / 32);
    }
    // 2. Into the probes.
    blendIrradiance_.dispatch(renderer, cb,
                              {paramsRes, VkComputeResource::image(rayData_.view()),
                               VkComputeResource::image(irradiance_.view()),
                               VkComputeResource::bufferRange(offsets_.buffer)},
                              nullptr, probes);
    blendDistance_.dispatch(renderer, cb,
                            {paramsRes, VkComputeResource::image(rayData_.view()),
                             VkComputeResource::image(distance_.view()),
                             VkComputeResource::bufferRange(offsets_.buffer)},
                            nullptr, probes);
    computeBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    // 3. Their borders.
    const std::int32_t tilesX = static_cast<std::int32_t>(counts_[0] * counts_[1]);
    const std::int32_t irradiancePush[2] = {6, tilesX};
    border_.dispatch(renderer, cb, {VkComputeResource::image(irradiance_.view())}, irradiancePush, probes);
    const std::int32_t distancePush[2] = {14, tilesX};
    border_.dispatch(renderer, cb, {VkComputeResource::image(distance_.view())}, distancePush, probes);
    computeBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ++framesSinceReset_;
}

}  // namespace iron
