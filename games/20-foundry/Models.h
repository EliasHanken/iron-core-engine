#pragma once

// Models from assets/models/<name>/<name>.gltf, loaded once each, with their
// textures shared. A model is a list of sections, one per material, ready to draw.

#include "asset/GltfLoader.h"
#include "core/Log.h"
#include "math/Aabb.h"
#include "math/Transform.h"
#include "render/Renderer.h"

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

class ModelLibrary {
public:
    ModelLibrary(iron::Renderer& renderer, std::string folder) : renderer_(renderer), folder_(std::move(folder)) {}

    // The model, loading it on first use. Null if it can't be read.
    const Model* get(const std::string& name, const iron::GltfStaticOptions& options = {}) {
        const auto found = models_.find(name);
        if (found != models_.end()) return found->second.get();
        auto& slot = models_[name];
        const auto loaded = iron::loadGltfStaticModel(folder_ + name + "/" + name + ".gltf", options);
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
            iron::Material& m = section.material;
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
            m.heightScale = 0.0f;
            model->sections.push_back(section);
        }
        const iron::Vec3 size = model->size();
        iron::Log::info("foundry: model %-26s %zu sections, %.2f x %.2f x %.2f m", name.c_str(),
                        model->sections.size(), size.x, size.y, size.z);
        slot = std::move(model);
        return slot.get();
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
    std::map<std::string, iron::TextureHandle> textures_;
};

}  // namespace foundry
