#pragma once

#include "math/Mat4.h"
#include "render/Light.h"
#include "render/Renderer.h"  // for MeshHandle, TextureHandle, kInvalidHandle

#include <vector>

namespace iron {

// One renderable thing in the world: where it is, which mesh, which texture.
// It has no shader — M2 draws every object with one shared lit shader.
struct RenderObject {
    Mat4 transform = Mat4::identity();
    MeshHandle mesh = kInvalidHandle;
    TextureHandle texture = kInvalidHandle;
};

// A drawable world: a flat list of objects plus the light they are lit by.
// Deliberately not an entity-component system — just a struct and a vector.
struct Scene {
    std::vector<RenderObject> objects;
    DirectionalLight light;
};

} // namespace iron
