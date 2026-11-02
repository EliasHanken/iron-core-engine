#pragma once

#include "math/Mat4.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace iron {

// How long one stretch of a frame took on the GPU. Scopes can nest, and `depth`
// says how deeply, so a frame breakdown can indent them.
struct GpuTiming {
    const char*   name = "";   // A string literal: scopes are named by code, not data.
    float         milliseconds = 0.0f;
    std::uint32_t depth = 0;
};

// The CPU side of a GPU profiler: which pair of timestamp queries brackets each
// named scope in one frame, and how raw ticks become milliseconds. It knows
// nothing about Vulkan, so it can be tested without a GPU.
class GpuScopeTable {
public:
    explicit GpuScopeTable(std::uint32_t maxScopes) : maxScopes_(maxScopes) {}

    void clear();

    // The query index to write a timestamp to as a scope opens, or -1 when the
    // table is full. A full table still balances: its end() also returns -1.
    int begin(const char* name);
    // The query index for the end of the innermost open scope, or -1.
    int end();

    // Queries used so far, starting at index 0.
    std::uint32_t queryCount() const { return static_cast<std::uint32_t>(scopes_.size()) * 2; }
    bool balanced() const { return open_.empty(); }

    // Turns `ticks` (queryCount() timestamps) into timings, in the order the scopes
    // opened. Scopes left open are dropped. `validBits` comes from the queue: the
    // counter wraps at that many bits, so differences are taken modulo it.
    std::vector<GpuTiming> resolve(std::span<const std::uint64_t> ticks,
                                   float nanosecondsPerTick, std::uint32_t validBits) const;

private:
    struct Scope {
        const char*   name;
        std::uint32_t depth;
        bool          closed;
    };

    std::uint32_t      maxScopes_;
    std::vector<Scope> scopes_;
    std::vector<int>   open_;       // Indices into scopes_, innermost last.
    int                overflow_ = 0;  // Scopes refused while full, still to be ended.
};

// Remembers each moving object's transform from the frame before, so that motion
// vectors can say how far its pixels moved. Objects are named by a stable id
// chosen by the game. One that wasn't drawn last frame reports no motion.
class MotionHistory {
public:
    // Last frame's transform for `id`, or `model` itself if it's new. Records
    // `model` for the next frame.
    Mat4 previous(std::uint32_t id, const Mat4& model);
    // Moves to the next frame. Anything not drawn this frame is forgotten.
    void endFrame();

    std::size_t tracked() const { return last_.size(); }

private:
    std::unordered_map<std::uint32_t, Mat4> last_;
    std::unordered_map<std::uint32_t, Mat4> current_;
};

// The same, for a skinned object's bone palette: last frame's, so the skinned
// motion vectors see the bones move as well as the object.
class PaletteHistory {
public:
    // Last frame's palette for `id`, or `palette` itself if it's new. Records
    // `palette` for the next frame. Valid until endFrame.
    std::span<const Mat4> previous(std::uint32_t id, std::span<const Mat4> palette);
    void endFrame();

    std::size_t tracked() const { return last_.size(); }

private:
    std::unordered_map<std::uint32_t, std::vector<Mat4>> last_;
    std::unordered_map<std::uint32_t, std::vector<Mat4>> current_;
};

}  // namespace iron
