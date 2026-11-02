#include "render/backends/vulkan/VkGpuProfiler.h"

#include "core/Log.h"
#include "render/backends/vulkan/VkContext.h"

#include <vector>

namespace iron {

namespace {

constexpr std::uint32_t kQueriesPerFrame = VkGpuProfiler::kMaxScopes * 2;

}  // namespace

bool VkGpuProfiler::init(VkContext& ctx) {
    static_assert(VkFrameRing::kFramesInFlight == 2, "tables_ is initialised for two frames");
    device_ = ctx.device();

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &properties);
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &familyCount, families.data());
    if (ctx.graphicsFamily() < familyCount) {
        validBits_ = families[ctx.graphicsFamily()].timestampValidBits;
    }
    if (validBits_ == 0 || properties.limits.timestampPeriod <= 0.0f) {
        Log::warn("VkGpuProfiler: this GPU can't time its graphics queue; GPU timings are off");
        return false;
    }
    nanosecondsPerTick_ = properties.limits.timestampPeriod;

    VkQueryPoolCreateInfo info{};
    info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kQueriesPerFrame * VkFrameRing::kFramesInFlight;
    if (vkCreateQueryPool(device_, &info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        Log::warn("VkGpuProfiler: could not create a timestamp query pool");
        return false;
    }
    ticks_.resize(kQueriesPerFrame);
    return true;
}

void VkGpuProfiler::destroy(VkContext& ctx) {
    if (pool_ != VK_NULL_HANDLE) vkDestroyQueryPool(ctx.device(), pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
}

void VkGpuProfiler::beginFrame(VkCommandBuffer cb, int frame) {
    frame_ = -1;
    if (pool_ == VK_NULL_HANDLE || frame < 0 || frame >= VkFrameRing::kFramesInFlight) return;
    frame_ = frame;
    GpuScopeTable& table = tables_[static_cast<std::size_t>(frame)];
    const std::uint32_t first = static_cast<std::uint32_t>(frame) * kQueriesPerFrame;

    // The caller waited on this slot's fence, so last time's queries are done.
    const std::uint32_t count = table.queryCount();
    if (recorded_[static_cast<std::size_t>(frame)] && count > 0 && table.balanced()) {
        const VkResult r = vkGetQueryPoolResults(device_, pool_, first, count,
                                                 count * sizeof(std::uint64_t), ticks_.data(),
                                                 sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
        if (r == VK_SUCCESS) {
            latest_ = table.resolve(std::span(ticks_.data(), count), nanosecondsPerTick_, validBits_);
        }
    }

    table.clear();
    recorded_[static_cast<std::size_t>(frame)] = true;
    vkCmdResetQueryPool(cb, pool_, first, kQueriesPerFrame);
}

void VkGpuProfiler::begin(VkCommandBuffer cb, const char* name) {
    if (frame_ < 0) return;
    const int query = tables_[static_cast<std::size_t>(frame_)].begin(name);
    if (query < 0) return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_,
                        static_cast<std::uint32_t>(frame_) * kQueriesPerFrame + static_cast<std::uint32_t>(query));
}

void VkGpuProfiler::end(VkCommandBuffer cb) {
    if (frame_ < 0) return;
    const int query = tables_[static_cast<std::size_t>(frame_)].end();
    if (query < 0) return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_,
                        static_cast<std::uint32_t>(frame_) * kQueriesPerFrame + static_cast<std::uint32_t>(query));
}

}  // namespace iron
