#include "scene/EntityJson.h"

#include "audio/AudioEmitter.h"
#include "gameplay/LogicGraphComponent.h"
#include "render/ReflectionProbe.h"
#include "scene/ReflectionIO.h"
#include "world/CollisionShape.h"
#include "world/ComponentRegistry.h"

#include <nlohmann/json.hpp>

namespace iron {

using json = nlohmann::json;

json vec3ToJson(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

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
    if (e.parentIndex != -1) j["parent"] = e.parentIndex;   // M69
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
    if (j.contains("parent") && j["parent"].is_number_integer())
        e.parentIndex = j["parent"].get<int>();   // validated after the whole scene/prefab loads

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

}  // namespace iron
