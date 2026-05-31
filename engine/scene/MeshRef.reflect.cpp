#include "reflection/Reflection.h"
#include "scene/SceneFormat.h"

namespace iron {

void registerMeshRef(Reflection& r) {
    r.registerEnum<PrimitiveKind>("PrimitiveKind")
        .value("cube",  PrimitiveKind::Cube)
        .value("plane", PrimitiveKind::Plane);
    r.registerType<MeshRef>("MeshRef")
        .field("primitive", &MeshRef::primitive)
        .field("gltfPath",  &MeshRef::gltfPath);
}

}  // namespace iron
