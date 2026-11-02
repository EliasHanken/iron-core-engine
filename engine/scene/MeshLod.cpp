#include "scene/MeshLod.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cstddef>

namespace iron {

std::vector<MeshData> generateLods(const MeshData& mesh, int levels, const LodSettings& settings) {
    std::vector<MeshData> out;
    if (mesh.vertices.empty() || mesh.indices.empty() || levels < 1) return out;

    MeshData base = mesh;
    meshopt_optimizeVertexCache(base.indices.data(), base.indices.data(), base.indices.size(),
                                base.vertices.size());
    out.push_back(base);

    const float* positions = &base.vertices[0].position.x;
    const std::size_t stride = sizeof(Vertex);
    std::vector<std::uint32_t> previous = base.indices;
    for (int level = 1; level < levels; ++level) {
        const std::size_t target =
            std::max<std::size_t>(static_cast<std::size_t>(static_cast<float>(previous.size()) * settings.reduction) / 3 * 3, 3);
        std::vector<std::uint32_t> simpler(previous.size());
        float error = 0.0f;
        // Each level simplifies the one before, so the chain stays consistent.
        const std::size_t count = meshopt_simplify(simpler.data(), previous.data(), previous.size(), positions,
                                                   base.vertices.size(), stride, target,
                                                   settings.maxError * static_cast<float>(level), 0, &error);
        if (count < 3 || static_cast<float>(count) > static_cast<float>(previous.size()) * settings.minGain) break;
        simpler.resize(count);
        meshopt_optimizeVertexCache(simpler.data(), simpler.data(), simpler.size(), base.vertices.size());
        MeshData lod;
        lod.vertices = base.vertices;
        lod.indices = simpler;
        out.push_back(std::move(lod));
        previous = std::move(simpler);
    }
    return out;
}

}  // namespace iron
