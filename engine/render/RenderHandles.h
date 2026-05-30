#pragma once

#include "render/Handles.h"

namespace iron {

struct RenderHandles {
    MeshHandle    mesh     = 0;
    TextureHandle albedo   = 0;
    TextureHandle normal   = 0;
    TextureHandle specular = 0;
};

}  // namespace iron
