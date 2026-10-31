#include "render/backends/vulkan/VkLightVolume.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"
#include "render/backends/vulkan/VulkanRenderer.h"

#include "core/Log.h"

namespace iron {

namespace {

constexpr std::uint32_t kLocalSize = 4;

const char* const kResolveGlsl = R"GLSL(#version 450
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;
layout(std430, set = 0, binding = 0) readonly buffer Light { uint light[]; };
layout(rgba16f, set = 0, binding = 1) writeonly uniform image3D result;
layout(push_constant) uniform Push { ivec4 size; float scale; } push;

void main() {
    ivec3 v = ivec3(gl_GlobalInvocationID);
    if (any(greaterThanEqual(v, push.size.xyz))) return;
    int i = 3 * (v.x + push.size.x * (v.y + push.size.y * v.z));
    vec3 color = vec3(light[i], light[i + 1], light[i + 2]) * push.scale;
    imageStore(result, v, vec4(color, 1.0));
}
)GLSL";

// One axis of a Gaussian. A wide kernel is what lets light reach empty voxels.
const char* const kBlurGlsl = R"GLSL(#version 450
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;
layout(rgba16f, set = 0, binding = 0) readonly uniform image3D source;
layout(rgba16f, set = 0, binding = 1) writeonly uniform image3D result;
layout(push_constant) uniform Push { ivec4 size; ivec4 axis; } push;

const float WEIGHTS[5] = float[5](0.2270270, 0.1945946, 0.1216216, 0.0540540, 0.0162162);

void main() {
    ivec3 v = ivec3(gl_GlobalInvocationID);
    if (any(greaterThanEqual(v, push.size.xyz))) return;
    vec3 sum = imageLoad(source, v).rgb * WEIGHTS[0];
    for (int i = 1; i < 5; ++i) {
        ivec3 offset = push.axis.xyz * i;
        sum += imageLoad(source, clamp(v + offset, ivec3(0), push.size.xyz - 1)).rgb * WEIGHTS[i];
        sum += imageLoad(source, clamp(v - offset, ivec3(0), push.size.xyz - 1)).rgb * WEIGHTS[i];
    }
    imageStore(result, v, vec4(sum, 1.0));
}
)GLSL";

struct ResolvePush {
    std::int32_t size[4];
    float        scale;
};

struct BlurPush {
    std::int32_t size[4];
    std::int32_t axis[4];
};

}  // namespace

VkLightVolume::~VkLightVolume() {
    if (!renderer_) return;
    VkContext& ctx = renderer_->context();
    vkDeviceWaitIdle(ctx.device());

    blurStage_.destroy(ctx);
    resolveStage_.destroy(ctx);
    images_[0].destroy(ctx);
    images_[1].destroy(ctx);
    if (accumulation_) vmaDestroyBuffer(ctx.allocator(), accumulation_, accumulationAlloc_);
}

bool VkLightVolume::init(VulkanRenderer& renderer, const LightVolumeDesc& desc) {
    renderer_ = &renderer;
    desc_ = desc;
    VkContext& ctx = renderer.context();

    VkBufferCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = static_cast<VkDeviceSize>(desc.size.x) * desc.size.y * desc.size.z * 3 * 4;
    info.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &alloc, &accumulation_, &accumulationAlloc_,
                             nullptr));

    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!images_[0].create(ctx, desc.size, VK_FORMAT_R16G16B16A16_SFLOAT, kUsage)) return false;
    if (!images_[1].create(ctx, desc.size, VK_FORMAT_R16G16B16A16_SFLOAT, kUsage)) return false;

    if (!resolveStage_.init(ctx, kResolveGlsl,
                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                            sizeof(ResolvePush))) {
        return false;
    }
    if (!blurStage_.init(ctx, kBlurGlsl,
                         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                         sizeof(BlurPush))) {
        return false;
    }

    // Each pass runs three axes, flipping between the two images.
    resolvedIndex_ = (desc.blurPasses * 3) % 2;
    return true;
}

void VkLightVolume::beginInject() {
    VkCommandBuffer cb = renderer_->currentCommandBuffer();
    if (cb == VK_NULL_HANDLE) return;

    // The previous frame may still be resolving from this buffer.
    VkMemoryBarrier toFill{};
    toFill.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toFill.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    toFill.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &toFill, 0, nullptr, 0, nullptr);

    vkCmdFillBuffer(cb, accumulation_, 0, VK_WHOLE_SIZE, 0);

    VkMemoryBarrier toInject{};
    toInject.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toInject.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toInject.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &toInject, 0, nullptr, 0, nullptr);
}

void VkLightVolume::resolve() {
    VkCommandBuffer cb = renderer_->currentCommandBuffer();
    if (cb == VK_NULL_HANDLE) return;

    computeToComputeBarrier(cb);

    const auto groups = [](std::uint32_t size) { return (size + kLocalSize - 1) / kLocalSize; };
    const std::uint32_t gx = groups(desc_.size.x);
    const std::uint32_t gy = groups(desc_.size.y);
    const std::uint32_t gz = groups(desc_.size.z);
    const std::int32_t sx = static_cast<std::int32_t>(desc_.size.x);
    const std::int32_t sy = static_cast<std::int32_t>(desc_.size.y);
    const std::int32_t sz = static_cast<std::int32_t>(desc_.size.z);

    images_[0].discard();
    images_[0].transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT);
    ResolvePush resolvePush{{sx, sy, sz, 0}, 1.0f / kFixedPointScale};
    resolveStage_.dispatch(*renderer_, cb,
                           {VkComputeResource::bufferRange(accumulation_),
                            VkComputeResource::image(images_[0].view())},
                           &resolvePush, gx, gy, gz);

    int source = 0;
    for (int pass = 0; pass < desc_.blurPasses; ++pass) {
        for (int axis = 0; axis < 3; ++axis) {
            VkVolumeImage& src = images_[source];
            VkVolumeImage& dst = images_[1 - source];
            src.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_READ_BIT);
            dst.discard();
            dst.transition(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT);

            BlurPush blurPush{{sx, sy, sz, 0}, {axis == 0, axis == 1, axis == 2, 0}};
            blurStage_.dispatch(*renderer_, cb,
                                {VkComputeResource::image(src.view()),
                                 VkComputeResource::image(dst.view())},
                                &blurPush, gx, gy, gz);
            source = 1 - source;
        }
    }

    images_[resolvedIndex_].transition(cb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_SHADER_READ_BIT);
}

std::unique_ptr<LightVolume> createLightVolume(Renderer& renderer, const LightVolumeDesc& desc) {
    auto* vk = dynamic_cast<VulkanRenderer*>(&renderer);
    if (!vk) {
        Log::error("createLightVolume: renderer is not a VulkanRenderer");
        return nullptr;
    }
    auto volume = std::make_unique<VkLightVolume>();
    if (!volume->init(*vk, desc)) return nullptr;
    return volume;
}

}  // namespace iron
