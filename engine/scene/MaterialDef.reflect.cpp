#include "reflection/Reflection.h"
#include "scene/SceneFormat.h"

namespace iron {

void registerMaterialDef(Reflection& r) {
    r.registerType<MaterialDef>("MaterialDef")
        .field("albedoPath",            &MaterialDef::albedoPath)
        .field("normalPath",            &MaterialDef::normalPath)
        .field("metallicRoughnessPath", &MaterialDef::metallicRoughnessPath)
        .field("aoPath",                &MaterialDef::aoPath)
        .field("metallic",  &MaterialDef::metallic,  {.min = 0.0f, .max = 1.0f, .slider = true})
        .field("roughness", &MaterialDef::roughness, {.min = 0.0f, .max = 1.0f, .slider = true})
        .field("ao",        &MaterialDef::ao,        {.min = 0.0f, .max = 1.0f, .slider = true})
        .field("emissive",     &MaterialDef::emissive,     {.color = true})
        .field("uvScale",      &MaterialDef::uvScale,      {.min = 0.0f, .max = 100.0f})
        .field("reflectivity", &MaterialDef::reflectivity, {.min = 0.0f, .max = 1.0f, .slider = true});
}

}  // namespace iron
