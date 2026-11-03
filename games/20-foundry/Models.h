#pragma once

// Models from assets/models/<name>/<name>.gltf, loaded once each, with their
// textures shared. A model is a list of sections, one per material, ready to draw.

#include "asset/GltfLoader.h"
#include "core/Log.h"
#include "math/Aabb.h"
#include "math/Transform.h"
#include "render/InstanceGroup.h"
#include "render/Renderer.h"
#include "scene/MeshLod.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace foundry {

struct ModelSection {
    iron::MeshHandle mesh = iron::kInvalidHandle;
    iron::Material   material;
    std::string      node;
};

struct Model {
    std::vector<ModelSection> sections;
    iron::Aabb bounds{};

    iron::Vec3 size() const { return bounds.max - bounds.min; }
    iron::Vec3 center() const { return (bounds.min + bounds.max) * 0.5f; }
};

// A model drawn as many copies on the GPU: an instance group per section, each with
// levels of detail. Every group takes the same transforms.
struct InstancedModel {
    std::vector<iron::InstanceGroupHandle> groups;
    iron::Aabb bounds{};
    std::size_t triangles = 0;  // At the finest level, all sections.

    iron::Vec3 size() const { return bounds.max - bounds.min; }
    iron::Vec3 center() const { return (bounds.min + bounds.max) * 0.5f; }
};

class ModelLibrary {
public:
    ModelLibrary(iron::Renderer& renderer, std::string folder) : renderer_(renderer), folder_(std::move(folder)) {}

    // The model, loading it on first use. Null if it can't be read.
    // `name` is a model's folder, or "folder#node" for one piece of a kit.
    const Model* get(const std::string& name, const iron::GltfStaticOptions& options = {}) {
        const auto found = models_.find(name);
        if (found != models_.end()) return found->second.get();
        auto& slot = models_[name];
        const std::size_t hash = name.find('#');
        const std::string folder = name.substr(0, hash);
        iron::GltfStaticOptions chosen = options;
        if (hash != std::string::npos) chosen.onlyNodes.push_back(name.substr(hash + 1));
        const auto loaded = iron::loadGltfStaticModel(folder_ + folder + "/" + folder + ".gltf", chosen);
        if (!loaded) {
            iron::Log::error("foundry: model '%s' didn't load", name.c_str());
            return nullptr;
        }
        auto model = std::make_unique<Model>();
        model->bounds = loaded->bounds;
        for (const iron::GltfSection& s : loaded->sections) {
            ModelSection section;
            section.mesh = renderer_.createMesh(s.mesh);
            section.node = s.node;
            section.material = materialFor(s);
            model->sections.push_back(section);
        }
        const iron::Vec3 size = model->size();
        iron::Log::info("foundry: model %-26s %zu sections, %.2f x %.2f x %.2f m", name.c_str(),
                        model->sections.size(), size.x, size.y, size.z);
        slot = std::move(model);
        return slot.get();
    }

    // The model as instance groups, loading it on first use. `lodPixels` are the
    // heights on screen at which each level gives way to the next.
    // `name` may be "folder#node", as for get(); `shadows` false for what's too
    // small or too far to cast one worth drawing.
    const InstancedModel* instanced(const std::string& name, iron::ShaderHandle shader,
                                    std::array<float, 3> lodPixels = {160.0f, 60.0f, 20.0f}, bool shadows = true) {
        const auto found = instanced_.find(name);
        if (found != instanced_.end()) return found->second.get();
        auto& slot = instanced_[name];
        const std::size_t hash = name.find('#');
        const std::string folder = name.substr(0, hash);
        iron::GltfStaticOptions chosen;
        if (hash != std::string::npos) chosen.onlyNodes.push_back(name.substr(hash + 1));
        const auto loaded = iron::loadGltfStaticModel(folder_ + folder + "/" + folder + ".gltf", chosen);
        if (!loaded) {
            iron::Log::error("foundry: model '%s' didn't load", name.c_str());
            return nullptr;
        }
        auto model = std::make_unique<InstancedModel>();
        model->bounds = loaded->bounds;
        for (const iron::GltfSection& s : loaded->sections) {
            iron::InstanceGroupDesc desc;
            for (const iron::MeshData& lod : iron::generateLods(s.mesh, 4)) desc.lods.push_back(renderer_.createMesh(lod));
            desc.lodPixels = lodPixels;
            desc.shader = shader;
            desc.material = materialFor(s);
            desc.bounds = loaded->bounds;  // Every section culls with the whole model.
            desc.castShadows = shadows;
            model->triangles += s.mesh.indices.size() / 3;
            const iron::InstanceGroupHandle group = renderer_.createInstanceGroup(desc);
            if (group != iron::kInvalidInstanceGroup) model->groups.push_back(group);
        }
        slot = std::move(model);
        return slot.get();
    }

    // A glTF material as the engine's, with its textures loaded once and shared.
    iron::Material materialFor(const iron::GltfSurface& s) {
        iron::Material m;
        m.texture = texture(s.materialPaths.albedo, true);
        m.normalMap = texture(s.materialPaths.normal, false);
        m.metallicRoughnessMap = texture(s.materialPaths.metalRoughness, false);
        m.aoMap = texture(s.materialPaths.occlusion, false);
        m.emissiveMap = texture(s.materialPaths.emissive, true);
        m.metallic = s.metallicFactor;
        m.roughness = s.roughnessFactor;
        m.baseColorFactor = s.baseColorFactor;
        m.emissive = s.emissiveFactor;
        m.normalScale = s.normalScale;
        m.alphaCutoff = s.alphaCutoff;
        m.heightScale = 0.0f;
        return m;
    }

private:
    iron::TextureHandle texture(const std::string& path, bool srgb) {
        if (path.empty()) return iron::kInvalidHandle;
        const std::string key = path + (srgb ? "|s" : "|l");
        const auto found = textures_.find(key);
        if (found != textures_.end()) return found->second;
        return textures_[key] = renderer_.loadTexture(path, srgb);
    }

    iron::Renderer& renderer_;
    std::string folder_;
    std::map<std::string, std::unique_ptr<Model>> models_;
    std::map<std::string, std::unique_ptr<InstancedModel>> instanced_;
    std::map<std::string, iron::TextureHandle> textures_;
};

}  // namespace foundry
