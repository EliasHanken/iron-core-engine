#pragma once

#include "math/Mat4.h"
#include "render/Fog.h"
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

// A drawable world: a flat list of objects plus the lights they are lit
// by — one directional sun, plus zero or more point lights — plus optional fog.
struct Scene {
    std::vector<RenderObject> objects;
    DirectionalLight light;
    std::vector<PointLight> pointLights;
    Fog fog;                       // density = 0 by default; fog is disabled
};

} // namespace iron
