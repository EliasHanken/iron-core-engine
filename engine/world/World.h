#pragma once

#include "world/Entity.h"

#include <cstdint>
#include <vector>

namespace iron {

class World {
public:
    EntityId create();
    void     destroy(EntityId e);
    bool     alive(EntityId e) const;

private:
    // generations_[i] == 0 means "slot never used"; non-zero is current gen.
    std::vector<uint32_t> generations_;
    std::vector<uint32_t> freeList_;
};

}  // namespace iron
