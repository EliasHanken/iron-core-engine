#include "asset/GltfLoader.h"
#include "test_framework.h"

#include <cmath>
#include <filesystem>
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

    // --- M22.5: BoxTextured.gltf has a material with a base-color texture ---
    {
        auto model = loadGltfModel(base + "/BoxTextured.gltf");
        CHECK(model.has_value());
        CHECK(!model->materialPaths.albedo.empty());
        // Path should resolve to CesiumLogoFlat.png (using filesystem helper
        // for cross-platform path comparison).
        CHECK(std::filesystem::path(model->materialPaths.albedo).filename().string()
              == "CesiumLogoFlat.png");
        // BoxTextured has no normal or metallic-roughness texture.
        CHECK(model->materialPaths.normal.empty());
        CHECK(model->materialPaths.metalRoughness.empty());
    }

    // --- M23: RiggedSimple.gltf has a skin with bones + weighted vertices ---
    {
        auto model = loadGltfModel(base + "/RiggedSimple.gltf");
        CHECK(model.has_value());
        CHECK(model->skinnedMesh.has_value());
        const auto& sm = *model->skinnedMesh;
        // RiggedSimple typically has 2 bones (root + child); some asset
        // variants may have 3. Accept either.
        CHECK(sm.skeleton.bones.size() >= 2);
        // Exactly one root + at least one child.
        int rootCount = 0;
        int childCount = 0;
        for (const auto& b : sm.skeleton.bones) {
            if (b.parentIndex < 0) ++rootCount;
            else ++childCount;
        }
        CHECK(rootCount == 1);
        CHECK(childCount >= 1);
        // Each vertex has weights summing to ~1.0 (normalized at load time).
        for (const auto& v : sm.vertices) {
            const float wsum = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
            CHECK(wsum > 0.99f && wsum < 1.01f);
        }
    }

    // --- M24: RiggedSimple.gltf has exactly one animation; at least one
    //     rotation channel must resolve to a real bone.
    {
        auto model = loadGltfModel(base + "/RiggedSimple.gltf");
        CHECK(model.has_value());
        if (model.has_value()) {
            CHECK(model->animations.size() == 1u);
            if (!model->animations.empty()) {
                const auto& clip = model->animations[0];
                CHECK(clip.duration > 0.0f);
                CHECK(!clip.channels.empty());
                CHECK(!clip.samplers.empty());
                bool foundRot = false;
                for (const auto& ch : clip.channels) {
                    if (ch.targetBone >= 0 && ch.path == AnimationPath::Rotation) {
                        foundRot = true;
                        break;
                    }
                }
                CHECK(foundRot);
            }
        }
    }

    // --- M25: findClip looks up an animation clip by name. RiggedSimple's
    //     one animation has an empty name in the glTF; exact-match lookup
    //     must still succeed (forces real string-equality, not a "name
    //     non-empty" shortcut). A miss returns nullptr.
    {
        auto model = loadGltfModel(base + "/RiggedSimple.gltf");
        CHECK(model.has_value());
        if (model.has_value()) {
            CHECK(!model->animations.empty());
            if (!model->animations.empty()) {
                const std::string& name = model->animations[0].name;
                const AnimationClip* found = model->findClip(name);
                CHECK(found == &model->animations[0]);
                CHECK(model->findClip("does-not-exist") == nullptr);
            }
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
