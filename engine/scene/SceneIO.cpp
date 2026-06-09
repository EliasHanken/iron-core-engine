#include "scene/SceneIO.h"

#include "audio/AudioEmitter.h"
#include "core/Log.h"
#include "gameplay/LogicGraphComponent.h"
#include "render/ReflectionProbe.h"
#include "scene/ReflectionIO.h"
#include "world/CollisionShape.h"
#include "world/ComponentRegistry.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>

namespace iron {

namespace {

using json = nlohmann::json;

json toJson(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

void readVec3(const json& j, const char* key, Vec3& out) {
    if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
        out.x = j[key][0].get<float>();
        out.y = j[key][1].get<float>();
        out.z = j[key][2].get<float>();
    }
}

void readFloat(const json& j, const char* key, float& out) {
    if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
}

void readString(const json& j, const char* key, std::string& out) {
    if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
}

json entityToJson(const Reflection& r, const ComponentRegistry& cr, const SceneEntity& e) {
    json j = json::object();
    j["name"]      = e.name;
    j["transform"] = componentToJson(r, e.transform);
    j["mesh"]      = componentToJson(r, e.mesh);
    j["material"]  = componentToJson(r, e.material);
    json comps = json::array();
    for (const auto& box : e.components.all()) {
        const ComponentRegistry::Entry* entry = cr.byTypeId(box->typeId());
        if (!entry) continue;
        json cj = componentToJsonByPtr(r, entry->fields, box->data());
        cj["type"] = std::string(entry->name);
        comps.push_back(std::move(cj));
    }
    if (!comps.empty()) j["components"] = std::move(comps);
    return j;
}

SceneEntity entityFromJson(const Reflection& r, const ComponentRegistry& cr, const json& j) {
    SceneEntity e;
    readString(j, "name", e.name);
    if (j.contains("transform")) componentFromJson(r, e.transform, j["transform"]);
    if (j.contains("mesh"))      componentFromJson(r, e.mesh,      j["mesh"]);
    if (j.contains("material"))  componentFromJson(r, e.material,  j["material"]);

    if (j.contains("components") && j["components"].is_array()) {
        for (const json& cj : j["components"]) {
            if (!cj.contains("type") || !cj["type"].is_string()) continue;
            const ComponentRegistry::Entry* entry = cr.byName(cj["type"].get<std::string>());
            if (!entry) continue;
            auto box = entry->factory();
            componentFromJsonByPtr(r, entry->fields, box->data(), cj);
            e.components.addBox(std::move(box));
        }
    }

    // Back-compat: legacy top-level keys -> components.
    auto legacy = [&](const char* key, std::string_view typeName) {
        if (!j.contains(key)) return;
        const ComponentRegistry::Entry* entry = cr.byName(typeName);
        if (!entry) return;
        auto box = entry->factory();
        componentFromJsonByPtr(r, entry->fields, box->data(), j[key]);
        e.components.addBox(std::move(box));
    };
    legacy("collision", "CollisionShape");
    legacy("audio",     "AudioEmitter");
    legacy("probe",     "ReflectionProbeDef");
    if (j.contains("logicGraph") && j["logicGraph"].is_string()) {
        if (const auto* entry = cr.byName("LogicGraphComponent")) {
            auto box = entry->factory();
            static_cast<LogicGraphComponent*>(box->data())->graph = j["logicGraph"].get<std::string>();
            e.components.addBox(std::move(box));
        }
    }
    return e;
}

}  // namespace

std::string sceneToJsonString(const Reflection& reflection,
                              const ComponentRegistry& cr,
                              const SceneFile& scene) {
    json root = json::object();
    root["clearColor"] = toJson(scene.clearColor);

    json sun = json::object();
    sun["direction"] = toJson(scene.sun.direction);
    sun["color"]     = toJson(scene.sun.color);
    sun["ambient"]   = scene.sun.ambient;
    root["sun"] = sun;

    json fog = json::object();
    fog["color"]   = toJson(scene.fog.color);
    fog["density"] = scene.fog.density;
    root["fog"] = fog;

    json pls = json::array();
    for (const auto& pl : scene.pointLights) {
        json j = json::object();
        j["position"]  = toJson(pl.position);
        j["color"]     = toJson(pl.color);
        j["intensity"] = pl.intensity;
        j["range"]     = pl.range;
        pls.push_back(j);
    }
    root["pointLights"] = pls;

    json ents = json::array();
    for (const auto& e : scene.entities) ents.push_back(entityToJson(reflection, cr, e));
    root["entities"] = ents;

    return root.dump();
}

std::optional<SceneFile> sceneFromJsonString(const Reflection& reflection,
                                             const ComponentRegistry& cr,
                                             const std::string& jsonStr) {
    json root;
    try {
        root = json::parse(jsonStr);
    } catch (const json::exception& e) {
        Log::error("SceneIO: parse error: %s", e.what());
        return std::nullopt;
    }

    SceneFile scene;
    readVec3(root, "clearColor", scene.clearColor);

    if (root.contains("sun")) {
        const json& sun = root["sun"];
        readVec3 (sun, "direction", scene.sun.direction);
        readVec3 (sun, "color",     scene.sun.color);
        readFloat(sun, "ambient",   scene.sun.ambient);
    }
    if (root.contains("fog")) {
        const json& fog = root["fog"];
        readVec3 (fog, "color",   scene.fog.color);
        readFloat(fog, "density", scene.fog.density);
    }
    if (root.contains("pointLights") && root["pointLights"].is_array()) {
        for (const auto& j : root["pointLights"]) {
            PointLight pl;
            readVec3 (j, "position",  pl.position);
            readVec3 (j, "color",     pl.color);
            readFloat(j, "intensity", pl.intensity);
            readFloat(j, "range",     pl.range);
            scene.pointLights.push_back(pl);
        }
    }
    if (root.contains("entities") && root["entities"].is_array()) {
        for (const auto& j : root["entities"]) {
            scene.entities.push_back(entityFromJson(reflection, cr, j));
        }
    }
    return scene;
}

bool saveSceneFile(const Reflection& reflection,
                   const ComponentRegistry& cr,
                   const SceneFile& scene,
                   const std::string& path) {
    std::ofstream f(path);
    if (!f) {
        Log::error("SceneIO: cannot open '%s' for writing", path.c_str());
        return false;
    }
    // Re-indent for the human-diffable on-disk file (the string serializer is
    // compact); schema is defined once, in sceneToJsonString.
    json root = json::parse(sceneToJsonString(reflection, cr, scene));
    f << root.dump(2);
    return true;
}

std::optional<SceneFile> loadSceneFile(const Reflection& reflection,
                                       const ComponentRegistry& cr,
                                       const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        Log::error("SceneIO: cannot open '%s'", path.c_str());
        return std::nullopt;
    }
    std::string contents((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return sceneFromJsonString(reflection, cr, contents);
}

}  // namespace iron
