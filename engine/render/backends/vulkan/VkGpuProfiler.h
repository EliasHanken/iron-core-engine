#pragma once

#include "render/GpuTimings.h"
#include "render/backends/vulkan/VkFrameRing.h"

#include <vulkan/vulkan.h>

#include <array>
#include <span>
#include <vector>

namespace iron {

class VkContext;

// Measures how long each stretch of a frame takes on the GPU, with timestamp
// queries. Each frame in flight has its own block of queries, read back once its
// fence has signalled, so results arrive a couple of frames late and never stall.
class VkGpuProfiler {
public:
    static constexpr std::uint32_t kMaxScopes = 48;

    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    // Call right after the frame's command buffer begins, outside any render pass:
    // reads what this frame slot measured last time, then resets its queries.
    void beginFrame(VkCommandBuffer cb, int frame);
    void begin(VkCommandBuffer cb, const char* name);
    void end(VkCommandBuffer cb);

    // The most recent complete frame. Empty when the GPU can't time its queue.
    std::span<const GpuTiming> latest() const { return latest_; }

private:
    VkDevice    device_ = VK_NULL_HANDLE;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    float       nanosecondsPerTick_ = 1.0f;
    std::uint32_t validBits_ = 0;
    int         frame_ = -1;

    std::array<GpuScopeTable, VkFrameRing::kFramesInFlight> tables_{
        GpuScopeTable(kMaxScopes), GpuScopeTable(kMaxScopes)};
    std::array<bool, VkFrameRing::kFramesInFlight> recorded_{};
    std::vector<std::uint64_t> ticks_;
    std::vector<GpuTiming>     latest_;
};

}  // namespace iron
