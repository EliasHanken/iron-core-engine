// VkFluidVolume.cpp — GPU fluid solver (see FluidVolume.h).

#include "render/backends/vulkan/VkFluidVolume.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace iron {

namespace {

constexpr std::uint32_t kLocalSize = 4;
constexpr std::size_t kMinParamsBytes = 16;

const char* const kHeader = R"GLSL(#version 450
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(push_constant) uniform FluidPush {
    ivec4 size;
    vec4  boundsMin;
    vec4  voxel;
    float dt;
    float time;
    float dissipation;
    float velocityDissipation;
    float vorticity;
    float maxSpeed;
} fluid;

ivec3 thisCell() { return ivec3(gl_GlobalInvocationID); }
bool outsideGrid(ivec3 v) { return any(greaterThanEqual(v, fluid.size.xyz)); }
vec3 cellWorld(ivec3 v) { return fluid.boundsMin.xyz + (vec3(v) + 0.5) * fluid.voxel.xyz; }
vec3 cellUvw(ivec3 v) { return (vec3(v) + 0.5) / vec3(fluid.size.xyz); }
vec3 metresToUvw() { return 1.0 / (vec3(fluid.size.xyz) * fluid.voxel.xyz); }
ivec3 clampCell(ivec3 v) { return clamp(v, ivec3(0), fluid.size.xyz - 1); }
)GLSL";

const char* const kObstacleMain = R"GLSL(
layout(r8, set = 0, binding = 0) writeonly uniform image3D solid;
layout(rgba16f, set = 0, binding = 1) uniform image3D velocity;

void main() {
    ivec3 v = thisCell();
    if (outsideGrid(v)) return;
    bool inside = fluidObstacle(cellWorld(v)) < 0.0;
    imageStore(solid, v, vec4(inside ? 1.0 : 0.0));
    if (inside) imageStore(velocity, v, vec4(0.0));
}
)GLSL";

const char* const kSourceMain = R"GLSL(
layout(rgba16f, set = 0, binding = 0) uniform image3D velocityImage;
layout(rg16f, set = 0, binding = 1) uniform image3D scalarsImage;

void main() {
    ivec3 v = thisCell();
    if (outsideGrid(v)) return;

    vec3 velocity = imageLoad(velocityImage, v).xyz;
    vec2 scalars = imageLoad(scalarsImage, v).rg;
    fluidSource(cellWorld(v), velocity, scalars.r, scalars.g);

    // One bad voxel would spread through the whole grid.
    if (any(isnan(velocity)) || any(isinf(velocity))) velocity = vec3(0.0);
    if (isnan(scalars.r) || isinf(scalars.r)) scalars.r = 0.0;
    if (isnan(scalars.g) || isinf(scalars.g)) scalars.g = 0.0;

    float speed = length(velocity);
    if (speed > fluid.maxSpeed) velocity *= fluid.maxSpeed / speed;

    imageStore(velocityImage, v, vec4(velocity, 0.0));
    imageStore(scalarsImage, v, vec4(clamp(scalars, vec2(0.0), vec2(8.0)), 0.0, 0.0));
}
)GLSL";

const char* const kAdvectVelocityGlsl = R"GLSL(
layout(set = 0, binding = 0) uniform sampler3D source;
layout(rgba16f, set = 0, binding = 1) writeonly uniform image3D result;

void main() {
    ivec3 v = thisCell();
    if (outsideGrid(v)) return;
    vec3 uvw = cellUvw(v);
    vec3 here = texture(source, uvw).xyz;
    vec3 back = uvw - here * fluid.dt * metresToUvw();
    vec3 advected = texture(source, back).xyz * exp(-fluid.velocityDissipation * fluid.dt);
    imageStore(result, v, vec4(advected, 0.0));
}
)GLSL";

const char* const kCurlGlsl = R"GLSL(
layout(rgba16f, set = 0, binding = 0) readonly uniform image3D velocity;
layout(rgba16f, set = 0, binding = 1) writeonly uniform image3D result;

void main() {
    ivec3 v = thisCell();
    if (outsideGrid(v)) return;
    vec3 l = imageLoad(velocity, clampCell(v - ivec3(1, 0, 0))).xyz;
    vec3 r = imageLoad(velocity, clampCell(v + ivec3(1, 0, 0))).xyz;
    vec3 d = imageLoad(velocity, clampCell(v - ivec3(0, 1, 0))).xyz;
    vec3 u = imageLoad(velocity, clampCell(v + ivec3(0, 1, 0))).xyz;
    vec3 b = imageLoad(velocity, clampCell(v - ivec3(0, 0, 1))).xyz;
    vec3 f = imageLoad(velocity, clampCell(v + ivec3(0, 0, 1))).xyz;
    vec3 h = 0.5 / fluid.voxel.xyz;
    vec3 curl = vec3((u.z - d.z) * h.y - (f.y - b.y) * h.z,
                     (f.x - b.x) * h.z - (r.z - l.z) * h.x,
                     (r.y - l.y) * h.x - (u.x - d.x) * h.y);
    imageStore(result, v, vec4(curl, length(curl)));
}
)GLSL";

// Vorticity confinement: puts back the small curls that advection smears out.
const char* const kConfineGlsl = R"GLSL(
layout(rgba16f, set = 0, binding = 0) readonly uniform image3D curl;
layout(rgba16f, set = 0, binding = 1) uniform image3D velocity;

void main() {
    ivec3 v = thisCell();
    if (outsideGrid(v)) return;
    float l = imageLoad(curl, clampCell(v - ivec3(1, 0, 0))).w;
    float r = imageLoad(curl, clampCell(v + ivec3(1, 0, 0))).w;
    float d = imageLoad(curl, clampCell(v - ivec3(0, 1, 0))).w;
    float u = imageLoad(curl, clampCell(v + ivec3(0, 1, 0))).w;
    float b = imageLoad(curl, clampCell(v - ivec3(0, 0, 1))).w;
    float f = imageLoad(curl, clampCell(v + ivec3(0, 0, 1))).w;
    vec3 gradient = vec3(r - l, u - d, f - b) * 0.5 / fluid.voxel.xyz;
    float magnitude = length(gradient);
    if (magnitude < 1e-5) return;
    vec3 force = cross(gradient / magnitude, imageLoad(curl, v).xyz) *
                 fluid.vorticity * fluid.voxel.x;
    imageStore(velocity, v, imageLoad(velocity, v) + vec4(force * fluid.dt, 0.0));
}
)GLSL";

const char* const kDivergenceGlsl = R"GLSL(
layout(rgba16f, set = 0, binding = 0) readonly uniform image3D velocity;
layout(r16f, set = 0, binding = 1) writeonly uniform image3D result;

void main() {
    ivec3 v = thisCell();
    if (outsideGrid(v)) return;
    float l = imageLoad(velocity, clampCell(v - ivec3(1, 0, 0))).x;
    float r = imageLoad(velocity, clampCell(v + ivec3(1, 0, 0))).x;
    float d = imageLoad(velocity, clampCell(v - ivec3(0, 1, 0))).y;
    float u = imageLoad(velocity, clampCell(v + ivec3(0, 1, 0))).y;
    float b = imageLoad(velocity, clampCell(v - ivec3(0, 0, 1))).z;
    float f = imageLoad(velocity, clampCell(v + ivec3(0, 0, 1))).z;
    vec3 h = 0.5 / fluid.voxel.xyz;
    imageStore(result, v, vec4((r - l) * h.x + (u - d) * h.y + (f - b) * h.z));
}
)GLSL";

// A solid neighbour reflects this cell's own pressure, so nothing flows into it.
const char* const kJacobiGlsl = R"GLSL(
layout(r16f, set = 0, binding = 0) readonly uniform image3D pressure;
layout(r16f, set = 0, binding = 1) readonly uniform image3D divergence;
layout(r8, set = 0, binding = 2) readonly uniform image3D solid;
layout(r16f, set = 0, binding = 3) writeonly uniform image3D result;

float neighbour(ivec3 v, float center) {
    ivec3 c = clampCell(v);
    return imageLoad(solid, c).r > 0.5 ? center : imageLoad(pressure, c).r;
}

void main() {
    ivec3 v = thisCell();
    if (outsideGrid(v)) return;
    float center = imageLoad(pressure, v).r;
    float sum = neighbour(v - ivec3(1, 0, 0), center) + neighbour(v + ivec3(1, 0, 0), center) +
                neighbour(v - ivec3(0, 1, 0), center) + neighbour(v + ivec3(0, 1, 0), center) +
                neighbour(v - ivec3(0, 0, 1), center) + neighbour(v + ivec3(0, 0, 1), center);
    float h = fluid.voxel.x;
    imageStore(result, v, vec4((sum - h * h * imageLoad(divergence, v).r) / 6.0));
}
)GLSL";

const char* const kProjectGlsl = R"GLSL(
layout(r16f, set = 0, binding = 0) readonly uniform image3D pressure;
layout(r8, set = 0, binding = 1) readonly uniform image3D solid;
layout(rgba16f, set = 0, binding = 2) uniform image3D velocity;

float neighbour(ivec3 v, float center) {
    ivec3 c = clampCell(v);
    return imageLoad(solid, c).r > 0.5 ? center : imageLoad(pressure, c).r;
}

void main() {
    ivec3 v = thisCell();
    if (outsideGrid(v)) return;
    if (imageLoad(solid, v).r > 0.5) {
        imageStore(velocity, v, vec4(0.0));
        return;
    }
    float center = imageLoad(pressure, v).r;
    vec3 gradient = vec3(neighbour(v + ivec3(1, 0, 0), center) - neighbour(v - ivec3(1, 0, 0), center),
                         neighbour(v + ivec3(0, 1, 0), center) - neighbour(v - ivec3(0, 1, 0), center),
                         neighbour(v + ivec3(0, 0, 1), center) - neighbour(v - ivec3(0, 0, 1), center)) *
                    0.5 / fluid.voxel.xyz;
    vec3 velocityHere = imageLoad(velocity, v).xyz - gradient;
    float speed = length(velocityHere);
    if (speed > fluid.maxSpeed) velocityHere *= fluid.maxSpeed / speed;
    imageStore(velocity, v, vec4(velocityHere, 0.0));
}
)GLSL";

const char* const kAdvectForwardGlsl = R"GLSL(
layout(rgba16f, set = 0, binding = 0) readonly uniform image3D velocity;
layout(set = 0, binding = 1) uniform sampler3D source;
layout(rg16f, set = 0, binding = 2) writeonly uniform image3D result;

void main() {
    ivec3 v = thisCell();
    if (outsideGrid(v)) return;
    vec3 back = cellUvw(v) - imageLoad(velocity, v).xyz * fluid.dt * metresToUvw();
    imageStore(result, v, texture(source, back));
}
)GLSL";

// MacCormack: advect the result back and fix half the error, which keeps detail
// that plain advection blurs away. Clamping to the cell it came from stops the
// correction from ringing.
const char* const kAdvectCorrectGlsl = R"GLSL(
layout(rgba16f, set = 0, binding = 0) readonly uniform image3D velocity;
layout(set = 0, binding = 1) uniform sampler3D original;
layout(set = 0, binding = 2) uniform sampler3D forward;
layout(rg16f, set = 0, binding = 3) writeonly uniform image3D result;

void main() {
    ivec3 v = thisCell();
    if (outsideGrid(v)) return;

    vec3 uvw = cellUvw(v);
    vec3 step = imageLoad(velocity, v).xyz * fluid.dt * metresToUvw();
    vec2 phi0 = texture(original, uvw).rg;
    vec2 phi1 = texture(forward, uvw).rg;
    vec2 phiBack = texture(forward, uvw + step).rg;
    vec2 corrected = phi1 + 0.5 * (phi0 - phiBack);

    vec3 cellSize = 1.0 / vec3(fluid.size.xyz);
    vec3 from = uvw - step;
    vec2 low = vec2(1e9);
    vec2 high = vec2(-1e9);
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                vec3 corner = (vec3(x, y, z) - 0.5) * cellSize;
                vec2 sampleValue = texture(original, from + corner).rg;
                low = min(low, sampleValue);
                high = max(high, sampleValue);
            }
        }
    }
    corrected = clamp(corrected, low, high);

    float keep = exp(-fluid.dissipation * fluid.dt);
    imageStore(result, v, vec4(corrected * keep, 0.0, 0.0));
}
)GLSL";

std::string composeGameStage(const FluidVolumeDesc& desc, const std::string& function,
                             const char* main) {
    return std::string(kHeader) + desc.commonGlsl + function + main;
}

std::string composeFixedStage(const char* body) { return std::string(kHeader) + body; }

}  // namespace

// Mirrors the GLSL FluidPush block.
struct VkFluidVolume::Push {
    std::int32_t size[4];
    float        boundsMin[4];
    float        voxel[4];
    float        dt;
    float        time;
    float        dissipation;
    float        velocityDissipation;
    float        vorticity;
    float        maxSpeed;
};

std::vector<std::string> composeFluidSources(const FluidVolumeDesc& desc) {
    return {
        composeGameStage(desc, desc.obstacleGlsl, kObstacleMain),
        composeGameStage(desc, desc.sourceGlsl, kSourceMain),
        composeFixedStage(kAdvectVelocityGlsl),
        composeFixedStage(kCurlGlsl),
        composeFixedStage(kConfineGlsl),
        composeFixedStage(kDivergenceGlsl),
        composeFixedStage(kJacobiGlsl),
        composeFixedStage(kProjectGlsl),
        composeFixedStage(kAdvectForwardGlsl),
        composeFixedStage(kAdvectCorrectGlsl),
    };
}

VkFluidVolume::~VkFluidVolume() {
    if (!renderer_) return;
    VkContext& ctx = renderer_->context();
    vkDeviceWaitIdle(ctx.device());

    for (VkComputeStage* stage :
         {&obstacleStage_, &sourceStage_, &advectVelocityStage_, &curlStage_, &confineStage_,
          &divergenceStage_, &jacobiStage_, &projectStage_, &advectForwardStage_,
          &advectCorrectStage_}) {
        stage->destroy(ctx);
    }
    for (VkVolumeImage& image : velocity_) image.destroy(ctx);
    for (VkVolumeImage& image : scalars_) image.destroy(ctx);
    for (VkVolumeImage& image : pressure_) image.destroy(ctx);
    divergence_.destroy(ctx);
    curl_.destroy(ctx);
    solid_.destroy(ctx);
}

bool VkFluidVolume::init(VulkanRenderer& renderer, const FluidVolumeDesc& desc) {
    renderer_ = &renderer;
    desc_ = desc;
    params_.assign(std::max(desc.paramsSize, kMinParamsBytes), 0);
    VkContext& ctx = renderer.context();

    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const auto make = [&](VkVolumeImage& image, VkFormat format) {
        return image.create(ctx, desc.size, format, kUsage);
    };
    for (VkVolumeImage& image : velocity_) {
        if (!make(image, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
    }
    for (VkVolumeImage& image : scalars_) {
        if (!make(image, VK_FORMAT_R16G16_SFLOAT)) return false;
    }
    for (VkVolumeImage& image : pressure_) {
        if (!make(image, VK_FORMAT_R16_SFLOAT)) return false;
    }
    if (!make(divergence_, VK_FORMAT_R16_SFLOAT) || !make(curl_, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !make(solid_, VK_FORMAT_R8_UNORM)) {
        return false;
    }

    const std::vector<std::string> sources = composeFluidSources(desc);
    constexpr VkDescriptorType kImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr VkDescriptorType kSampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    constexpr VkDescriptorType kUniform = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constexpr std::uint32_t kPush = sizeof(Push);

    return obstacleStage_.init(ctx, sources[0], {kImage, kImage, kUniform}, kPush) &&
           sourceStage_.init(ctx, sources[1], {kImage, kImage, kUniform}, kPush) &&
           advectVelocityStage_.init(ctx, sources[2], {kSampled, kImage}, kPush) &&
           curlStage_.init(ctx, sources[3], {kImage, kImage}, kPush) &&
           confineStage_.init(ctx, sources[4], {kImage, kImage}, kPush) &&
           divergenceStage_.init(ctx, sources[5], {kImage, kImage}, kPush) &&
           jacobiStage_.init(ctx, sources[6], {kImage, kImage, kImage, kImage}, kPush) &&
           projectStage_.init(ctx, sources[7], {kImage, kImage, kImage}, kPush) &&
           advectForwardStage_.init(ctx, sources[8], {kImage, kSampled, kImage}, kPush) &&
           advectCorrectStage_.init(ctx, sources[9], {kImage, kSampled, kSampled, kImage}, kPush);
}

void VkFluidVolume::setParams(const void* params) {
    if (desc_.paramsSize > 0) std::memcpy(params_.data(), params, desc_.paramsSize);
}

VkDeviceSize VkFluidVolume::uploadParams() const {
    return renderer_->frameRing().allocateUbo(params_.data(), params_.size());
}

void VkFluidVolume::clearAll(VkCommandBuffer cb) {
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const auto clear = [&](VkVolumeImage& image) {
        image.discard();
        image.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cb, image.image(), VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    };
    for (VkVolumeImage& image : velocity_) clear(image);
    for (VkVolumeImage& image : scalars_) clear(image);
    for (VkVolumeImage& image : pressure_) clear(image);
    clear(divergence_);
    clear(curl_);
    clear(solid_);

    VkMemoryBarrier toCompute{};
    toCompute.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &toCompute, 0, nullptr, 0, nullptr);
}

void VkFluidVolume::step() {
    VkCommandBuffer cb = renderer_->currentCommandBuffer();
    if (cb == VK_NULL_HANDLE) return;

    // The previous frame's raymarch and particles may still be reading the grids.
    VkMemoryBarrier toStep{};
    toStep.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toStep.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toStep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toStep, 0, nullptr, 0,
                         nullptr);

    const bool firstStep = needsClear_;
    if (needsClear_) {
        clearAll(cb);
        needsClear_ = false;
    }

    time_ += kFluidStepSeconds;

    const Vec3 voxel = volumeVoxelSize(desc_.bounds, desc_.size);
    Push push{};
    push.size[0] = static_cast<std::int32_t>(desc_.size.x);
    push.size[1] = static_cast<std::int32_t>(desc_.size.y);
    push.size[2] = static_cast<std::int32_t>(desc_.size.z);
    push.boundsMin[0] = desc_.bounds.min.x;
    push.boundsMin[1] = desc_.bounds.min.y;
    push.boundsMin[2] = desc_.bounds.min.z;
    push.voxel[0] = voxel.x;
    push.voxel[1] = voxel.y;
    push.voxel[2] = voxel.z;
    push.dt = kFluidStepSeconds;
    push.time = time_;
    push.dissipation = dissipation_;
    push.velocityDissipation = desc_.velocityDissipation;
    push.vorticity = desc_.vorticity;
    push.maxSpeed = desc_.maxSpeed;

    const auto groups = [](std::uint32_t size) { return (size + kLocalSize - 1) / kLocalSize; };
    const std::uint32_t gx = groups(desc_.size.x);
    const std::uint32_t gy = groups(desc_.size.y);
    const std::uint32_t gz = groups(desc_.size.z);

    using R = VkComputeResource;
    const VkSampler sampler = renderer_->volumeSampler();
    const auto sampled = [&](const VkVolumeImage& image) {
        return R::sampled(image.view(), sampler, VK_IMAGE_LAYOUT_GENERAL);
    };
    const auto image = [](const VkVolumeImage& i) { return R::image(i.view()); };

    VkFrameRing::Frame& frame = renderer_->frameRing().current();
    const VkDeviceSize paramsOffset = uploadParams();
    const R params = R::bufferRange(frame.uboBuffer, paramsOffset, params_.size());

    const auto interval = static_cast<std::uint32_t>(std::max(desc_.obstacleInterval, 1));
    if (firstStep || stepIndex_ % interval == 0) {
        obstacleStage_.dispatch(*renderer_, cb, {image(solid_), image(velocity_[0]), params}, &push,
                                gx, gy, gz);
        computeToComputeBarrier(cb);
    }
    ++stepIndex_;

    sourceStage_.dispatch(*renderer_, cb, {image(velocity_[0]), image(scalars_[0]), params}, &push,
                          gx, gy, gz);
    computeToComputeBarrier(cb);

    advectVelocityStage_.dispatch(*renderer_, cb, {sampled(velocity_[0]), image(velocity_[1])},
                                  &push, gx, gy, gz);
    computeToComputeBarrier(cb);
    std::swap(velocity_[0], velocity_[1]);

    curlStage_.dispatch(*renderer_, cb, {image(velocity_[0]), image(curl_)}, &push, gx, gy, gz);
    computeToComputeBarrier(cb);
    confineStage_.dispatch(*renderer_, cb, {image(curl_), image(velocity_[0])}, &push, gx, gy, gz);
    computeToComputeBarrier(cb);

    divergenceStage_.dispatch(*renderer_, cb, {image(velocity_[0]), image(divergence_)}, &push, gx,
                              gy, gz);
    computeToComputeBarrier(cb);

    // Keeping last step's pressure as the starting guess lets a small iteration
    // count converge over several frames.
    for (int i = 0; i < desc_.pressureIterations; ++i) {
        jacobiStage_.dispatch(*renderer_, cb,
                              {image(pressure_[0]), image(divergence_), image(solid_),
                               image(pressure_[1])},
                              &push, gx, gy, gz);
        computeToComputeBarrier(cb);
        std::swap(pressure_[0], pressure_[1]);
    }

    projectStage_.dispatch(*renderer_, cb, {image(pressure_[0]), image(solid_), image(velocity_[0])},
                           &push, gx, gy, gz);
    computeToComputeBarrier(cb);

    advectForwardStage_.dispatch(*renderer_, cb,
                                 {image(velocity_[0]), sampled(scalars_[0]), image(scalars_[1])},
                                 &push, gx, gy, gz);
    computeToComputeBarrier(cb);
    advectCorrectStage_.dispatch(
        *renderer_, cb,
        {image(velocity_[0]), sampled(scalars_[0]), sampled(scalars_[1]), image(scalars_[2])}, &push,
        gx, gy, gz);
    std::swap(scalars_[0], scalars_[2]);

    // The raymarch reads the result from compute, and particles from their vertex
    // shaders, so this step must be visible to both.
    VkMemoryBarrier toRead{};
    toRead.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &toRead, 0, nullptr, 0, nullptr);
    hasStepped_ = true;
}

std::unique_ptr<FluidVolume> createFluidVolume(Renderer& renderer, const FluidVolumeDesc& desc) {
    auto* vk = dynamic_cast<VulkanRenderer*>(&renderer);
    if (!vk) {
        Log::error("createFluidVolume: renderer is not a VulkanRenderer");
        return nullptr;
    }
    auto volume = std::make_unique<VkFluidVolume>();
    if (!volume->init(*vk, desc)) return nullptr;
    return volume;
}

}  // namespace iron
