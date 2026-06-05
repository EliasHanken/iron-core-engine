#pragma once

#include "world/Entity.h"

#include <cstddef>
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

    // Iterate live (entity, component) pairs. f: void(EntityId, T&).
    template <class F>
    void forEach(F&& f) {
        for (std::size_t i = 0; i < dense_.size(); ++i) f(denseEntities_[i], dense_[i]);
    }

    void remove(EntityId e) {
        if (e.index >= sparse_.size()) return;
        const uint32_t row = sparse_[e.index];
        if (row == kNoRow) return;
        if (!(denseEntities_[row] == e)) return;  // stale handle

        const uint32_t last = static_cast<uint32_t>(dense_.size() - 1);
        if (row != last) {
            // Swap-and-pop: move the last entry into the freed row, then update
            // the swapped entity's sparse index to point at its new row.
            dense_[row]         = std::move(dense_[last]);
            denseEntities_[row] = denseEntities_[last];
            sparse_[denseEntities_[row].index] = row;
        }
        dense_.pop_back();
        denseEntities_.pop_back();
        sparse_[e.index] = kNoRow;
    }

    T&       operator[](size_t denseRow)       { return dense_[denseRow]; }
    const T& operator[](size_t denseRow) const { return dense_[denseRow]; }

    EntityId entityAt(size_t denseRow) const { return denseEntities_[denseRow]; }

protected:
    std::vector<T>        dense_;
    std::vector<EntityId> denseEntities_;
    std::vector<uint32_t> sparse_;
};

}  // namespace iron
