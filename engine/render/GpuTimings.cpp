#include "render/GpuTimings.h"

namespace iron {

void GpuScopeTable::clear() {
    scopes_.clear();
    open_.clear();
    overflow_ = 0;
}

int GpuScopeTable::begin(const char* name) {
    if (scopes_.size() >= maxScopes_) {
        ++overflow_;
        return -1;
    }
    const int index = static_cast<int>(scopes_.size());
    scopes_.push_back({name, static_cast<std::uint32_t>(open_.size()), false});
    open_.push_back(index);
    return index * 2;
}

int GpuScopeTable::end() {
    // The scopes refused while full opened last, so they close first.
    if (overflow_ > 0) {
        --overflow_;
        return -1;
    }
    if (open_.empty()) return -1;
    const int index = open_.back();
    open_.pop_back();
    scopes_[static_cast<std::size_t>(index)].closed = true;
    return index * 2 + 1;
}

std::vector<GpuTiming> GpuScopeTable::resolve(std::span<const std::uint64_t> ticks,
                                              float nanosecondsPerTick,
                                              std::uint32_t validBits) const {
    const std::uint64_t mask = validBits >= 64 ? ~0ull : (1ull << validBits) - 1ull;
    std::vector<GpuTiming> timings;
    timings.reserve(scopes_.size());
    for (std::size_t i = 0; i < scopes_.size(); ++i) {
        const Scope& scope = scopes_[i];
        if (!scope.closed || i * 2 + 1 >= ticks.size()) continue;
        const std::uint64_t elapsed = (ticks[i * 2 + 1] - ticks[i * 2]) & mask;
        const double ms = static_cast<double>(elapsed) * nanosecondsPerTick * 1e-6;
        timings.push_back({scope.name, static_cast<float>(ms), scope.depth});
    }
    return timings;
}

Mat4 MotionHistory::previous(std::uint32_t id, const Mat4& model) {
    current_[id] = model;
    const auto found = last_.find(id);
    return found != last_.end() ? found->second : model;
}

void MotionHistory::endFrame() {
    last_.swap(current_);
    current_.clear();
}

}  // namespace iron
