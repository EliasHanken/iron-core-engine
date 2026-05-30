#pragma once

#include "world/Entity.h"

#include <cstdint>
#include <vector>

namespace iron {

// Per-component-type storage: dense vector for cache-friendly iteration,
// sparse index from EntityId.index -> dense row. v1 covers add / get only;
// remove and iteration land in later tasks.
template <class T>
class ComponentArray {
public:
    static constexpr uint32_t kNoRow = UINT32_MAX;

    T* add(EntityId e, const T& value = {}) {
        if (e.index >= sparse_.size()) sparse_.resize(e.index + 1, kNoRow);
        // Overwrite if already present (defined behaviour for this v1).
        if (sparse_[e.index] != kNoRow) {
            dense_[sparse_[e.index]] = value;
            return &dense_[sparse_[e.index]];
        }
        sparse_[e.index] = static_cast<uint32_t>(dense_.size());
        dense_.push_back(value);
        denseEntities_.push_back(e);
        return &dense_.back();
    }

    T* get(EntityId e) {
        if (e.index >= sparse_.size()) return nullptr;
        const uint32_t row = sparse_[e.index];
        if (row == kNoRow) return nullptr;
        // Guard against generation mismatch (stale handle into recycled slot).
        if (!(denseEntities_[row] == e)) return nullptr;
        return &dense_[row];
    }

    const T* get(EntityId e) const {
        return const_cast<ComponentArray*>(this)->get(e);
    }

    size_t size() const { return dense_.size(); }

protected:
    std::vector<T>        dense_;
    std::vector<EntityId> denseEntities_;
    std::vector<uint32_t> sparse_;
};

}  // namespace iron
