#include "reflection/Reflection.h"
#include "world/Transform.h"

namespace iron {

void registerTransform(Reflection& r) {
    r.registerType<Transform>("Transform")
        .field("position", &Transform::position)
        .field("rotation", &Transform::rotation)
        .field("scale",    &Transform::scale, {.min = 0.001f});
}

}  // namespace iron
