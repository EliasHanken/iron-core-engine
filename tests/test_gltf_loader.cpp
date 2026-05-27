#include "asset/GltfLoader.h"
#include "test_framework.h"

#include <cmath>
#include <string>

int main() {
    using namespace iron;

    const std::string base = std::string(IRON_REPO_ROOT) + "/tests/assets/gltf";

    // --- Box.gltf: 24 vertices, 36 indices (one cube primitive with per-face
    //     normals). Positions are within [-0.5, 0.5] on each axis.
    {
        auto data = loadGltfMesh(base + "/Box.gltf");
        CHECK(data.has_value());
        if (data.has_value()) {
            CHECK(data->vertices.size() == 24);
            CHECK(data->indices.size()  == 36);
            for (const auto& v : data->vertices) {
                CHECK(std::fabs(v.position.x) <= 0.6f);
                CHECK(std::fabs(v.position.y) <= 0.6f);
                CHECK(std::fabs(v.position.z) <= 0.6f);
            }
        }
    }

    // --- BoxTextured.gltf: same geometry, but with TEXCOORD_0 populated.
    //     At least one UV must differ from (0, 0).
    {
        auto data = loadGltfMesh(base + "/BoxTextured.gltf");
        CHECK(data.has_value());
        if (data.has_value()) {
            CHECK(data->vertices.size() == 24);
            bool sawNonZeroUv = false;
            for (const auto& v : data->vertices) {
                if (v.uv.x != 0.0f || v.uv.y != 0.0f) { sawNonZeroUv = true; break; }
            }
            CHECK(sawNonZeroUv);
        }
    }

    // --- Triangle.gltf: the Khronos sample has only POSITION + indices, no
    //     NORMAL. Our loader requires NORMAL, so this must return nullopt.
    //     This exercises the "missing required attribute" error path.
    {
        auto data = loadGltfMesh(base + "/Triangle.gltf");
        CHECK(!data.has_value());
    }

    // --- Invalid path returns nullopt (parse failure path).
    {
        auto data = loadGltfMesh("/this/path/does/not/exist.gltf");
        CHECK(!data.has_value());
    }

    return iron_test_result();
}
