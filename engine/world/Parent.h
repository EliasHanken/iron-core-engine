#pragma once

#include "world/Entity.h"

namespace iron {

// M69: runtime parent link, mirrored from SceneEntity::parentIndex during
// spawn/rebuild. Engine-internal like RenderHandles — NOT in ComponentRegistry,
// never serialized, never shown in the Inspector. Render/picking compose model
// matrices by walking this chain so a Play-mode parent moved by physics or a
// logic graph drags its subtree (those updates live only in the World).
struct Parent {
    EntityId parent = kEntityNone;
};

}  // namespace iron
