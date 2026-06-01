#pragma once

#include "audio/AudioEmitter.h"
#include "math/Vec.h"
#include "render/Fog.h"
#include "render/Light.h"
#include "world/CollisionShape.h"
#include "world/Transform.h"

#include <optional>
#include <string>
#include <vector>

namespace iron {

// A builtin procedural mesh. v1 supports the two the engine already has
// builders for (makeCube / appendQuad). Sphere etc. are future additions.
enum class PrimitiveKind { Cube, Plane };

// How an entity gets its geometry: a builtin primitive OR a static
// (non-skinned) glTF path. If `primitive` is set it wins; otherwise
// `gltfPath` is loaded. If neither resolves, the loader logs and skips
// the entity (the rest of the scene still renders).
struct MeshRef {
    std::optional<PrimitiveKind> primitive;
    std::string                  gltfPath;
};

// Surface appearance. Texture paths resolve to engine textures at load;
// "" means "use the engine's builtin default" (white / flat-normal / no-spec).
struct MaterialDef {
    std::string albedoPath;
    std::string normalPath;
    std::string specularPath;
    Vec3        emissive     = {0.0f, 0.0f, 0.0f};
    float       uvScale      = 1.0f;
    float       reflectivity = 0.0f;
};

// One placed object: a transform + what to draw. The transform is M37's
// iron::Transform component (M39 unifies SceneEntity with the World's
// component model — see iron-core-engine-progress).
struct SceneEntity {
    std::string name;
    Transform   transform;
    MeshRef     mesh;
    MaterialDef material;
    std::optional<CollisionShape> collision;  // M42 — absent = no collider
    std::optional<AudioEmitter>   audio;       // M42 — absent = no emitter
};

// A complete authored scene: placed entities + global lighting/environment.
// Ambient lives on `sun.ambient` (DirectionalLight carries it) — there is
// no separate scene-level ambient.
struct SceneFile {
    std::vector<SceneEntity> entities;
    DirectionalLight         sun;
    std::vector<PointLight>  pointLights;
    Fog                      fog;
    Vec3                     clearColor = {0.5f, 0.6f, 0.7f};
};

}  // namespace iron
