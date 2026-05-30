#include "world/World.h"

namespace iron {

EntityId World::create() {
    uint32_t index;
    if (!freeList_.empty()) {
        index = freeList_.back();
        freeList_.pop_back();
        // generation was already bumped on destroy.
    } else {
        index = static_cast<uint32_t>(generations_.size());
        generations_.push_back(1);   // first-ever generation is 1
    }
    return EntityId{index, generations_[index]};
}

void World::destroy(EntityId e) {
    if (!alive(e)) return;
    // Bump generation; any stale handle that captured the old generation
    // will fail alive() and get<T>() from now on.
    ++generations_[e.index];
    if (generations_[e.index] == 0) generations_[e.index] = 1;  // wrap guard
    freeList_.push_back(e.index);
}

bool World::alive(EntityId e) const {
    if (e.index >= generations_.size()) return false;
    return e.valid() && generations_[e.index] == e.generation;
}

}  // namespace iron
