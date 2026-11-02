#pragma once

#include "scene/Mesh.h"

#include <vector>

namespace iron {

struct LodSettings {
    // Each level keeps this share of the level before's triangles, roughly.
    float reduction = 0.45f;
    // How far a simplified surface may stray from the original, as a share of the
    // mesh's size. Higher lets a level simplify further when the target can't be met.
    float maxError = 0.02f;
    // Stop when a level can't get at least this much smaller than the one before.
    float minGain = 0.85f;
};

// A chain of simpler versions of `mesh`, finest first, starting with `mesh` itself
// (vertex-cache optimised). Fewer than `levels` come back when a mesh won't simplify
// further, so a box stays one level. Every level keeps the full vertex list and
// differs only in its indices, so levels could share one vertex buffer; today each
// becomes a mesh of its own.
std::vector<MeshData> generateLods(const MeshData& mesh, int levels, const LodSettings& settings = {});

}  // namespace iron
