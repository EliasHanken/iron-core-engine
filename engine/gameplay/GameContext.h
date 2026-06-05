#pragma once

#include "world/Entity.h"

namespace iron {

class World;

// The gameplay-domain context a logic graph runs against. Set into
// RunContext::domainContext (as a void*) by the runtime each tick.
struct GameContext {
    World*   world = nullptr;
    EntityId self = {};        // the entity owning the running graph
    float    time = 0.0f;      // elapsed Play seconds
    float    deltaTime = 0.0f;
};

}  // namespace iron
