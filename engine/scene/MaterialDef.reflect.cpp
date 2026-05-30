#include "reflection/Reflection.h"
#include "scene/SceneFormat.h"

namespace iron {

void registerMaterialDef(Reflection& r) {
    r.registerType<MaterialDef>("MaterialDef")
        .field("albedoPath",   &MaterialDef::albedoPath)
        .field("normalPath",   &MaterialDef::normalPath)
        .field("specularPath", &MaterialDef::specularPath)
        .field("emissive",     &MaterialDef::emissive)
        .field("uvScale",      &MaterialDef::uvScale,      {.min = 0.0f, .max = 100.0f})
        .field("reflectivity", &MaterialDef::reflectivity, {.min = 0.0f, .max = 1.0f});
}

}  // namespace iron
