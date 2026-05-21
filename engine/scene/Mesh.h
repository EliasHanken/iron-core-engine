#pragma once

#include "math/Vec.h"

#include <cstdint>
#include <vector>

namespace iron {

// One vertex of a renderable mesh: position, normal, and texture coordinate.
struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

// CPU-side mesh: vertices plus an index list describing triangles. Uploaded to
// the GPU by the renderer (see Renderer::createMesh).
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

// A unit cube centered at the origin (side length 1), with per-face normals and
// UVs so every face can be textured. 24 vertices (4 per face), 36 indices.
MeshData makeCube();

} // namespace iron
