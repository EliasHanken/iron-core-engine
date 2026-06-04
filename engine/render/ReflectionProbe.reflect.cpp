#include "render/ReflectionProbe.h"
#include "reflection/Reflection.h"

namespace iron {

void registerReflectionProbe(Reflection& r) {
    r.registerType<ReflectionProbeDef>("ReflectionProbeDef")
        .field("halfExtents", &ReflectionProbeDef::halfExtents, {.min = 0.1f})
        .field("faceSize",    &ReflectionProbeDef::faceSize, {.min = 16})
        .field("intensity",   &ReflectionProbeDef::intensity, {.min = 0.0f, .max = 4.0f, .slider = true});
}

}  // namespace iron
