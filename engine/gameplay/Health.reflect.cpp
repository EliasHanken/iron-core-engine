#include "gameplay/Health.h"
#include "reflection/Reflection.h"

namespace iron {

void registerHealth(Reflection& r) {
    r.registerType<Health>("Health")
        .field("current", &Health::current)
        .field("max",     &Health::max);
}

}  // namespace iron
