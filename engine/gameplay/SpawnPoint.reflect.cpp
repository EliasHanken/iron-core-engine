#include "gameplay/SpawnPoint.h"
#include "reflection/Reflection.h"

namespace iron {

void registerSpawnPoint(Reflection& r) {
    r.registerType<SpawnPoint>("SpawnPoint")
        .field("group",   &SpawnPoint::group)
        .field("enabled", &SpawnPoint::enabled);
}

}  // namespace iron
