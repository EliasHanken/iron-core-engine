#include "scene/SceneIO.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace iron {

namespace {

using json = nlohmann::json;

json toJson(const Vec3& v) { return json::array({v.x, v.y, v.z}); }
json toJson(const Quat& q) { return json::array({q.x, q.y, q.z, q.w}); }

void readVec3(const json& j, const char* key, Vec3& out) {
    if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
        out.x = j[key][0].get<float>();
        out.y = j[key][1].get<float>();
        out.z = j[key][2].get<float>();
    }
}

void readQuat(const json& j, const char* key, Quat& out) {
    if (j.contains(key) && j[key].is_array() && j[key].size() == 4) {
        out.x = j[key][0].get<float>();
        out.y = j[key][1].get<float>();
        out.z = j[key][2].get<float>();
        out.w = j[key][3].get<float>();
    }
}

void readFloat(const json& j, const char* key, float& out) {
    if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
}

void readString(const json& j, const char* key, std::string& out) {
    if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
}

const char* primitiveName(PrimitiveKind k) {
    switch (k) {
        case PrimitiveKind::Cube:  return "cube";
        case PrimitiveKind::Plane: return "plane";
    }
    return "cube";
}

json materialToJson(const MaterialDef& m) {
    json j = json::object();
    if (!m.albedoPath.empty())   j["albedoPath"]   = m.albedoPath;
    if (!m.normalPath.empty())   j["normalPath"]   = m.normalPath;
    if (!m.specularPath.empty()) j["specularPath"] = m.specularPath;
    j["emissive"]     = toJson(m.emissive);
    j["uvScale"]      = m.uvScale;
    j["reflectivity"] = m.reflectivity;
    return j;
}

json meshToJson(const MeshRef& m) {
    json j = json::object();
    if (m.primitive.has_value()) {
        j["primitive"] = primitiveName(m.primitive.value());
    } else {
        j["gltfPath"] = m.gltfPath;
    }
    return j;
}

json entityToJson(const SceneEntity& e) {
    json j = json::object();
    j["name"]     = e.name;
    j["position"] = toJson(e.transform.position);
    j["rotation"] = toJson(e.transform.rotation);
    j["scale"]    = toJson(e.transform.scale);
    j["mesh"]     = meshToJson(e.mesh);
    j["material"] = materialToJson(e.material);
    return j;
}

MaterialDef materialFromJson(const json& j) {
    MaterialDef m;
    readString(j, "albedoPath",   m.albedoPath);
    readString(j, "normalPath",   m.normalPath);
    readString(j, "specularPath", m.specularPath);
    readVec3  (j, "emissive",     m.emissive);
    readFloat (j, "uvScale",      m.uvScale);
    readFloat (j, "reflectivity", m.reflectivity);
    return m;
}

MeshRef meshFromJson(const json& j) {
    MeshRef m;
    if (j.contains("primitive") && j["primitive"].is_string()) {
        const std::string p = j["primitive"].get<std::string>();
        if (p == "cube")       m.primitive = PrimitiveKind::Cube;
        else if (p == "plane") m.primitive = PrimitiveKind::Plane;
        else Log::warn("SceneIO: unknown primitive '%s'; treating as gltf/none", p.c_str());
    }
    readString(j, "gltfPath", m.gltfPath);
    return m;
}

SceneEntity entityFromJson(const json& j) {
    SceneEntity e;
    readString(j, "name", e.name);
    readVec3  (j, "position", e.transform.position);
    readQuat  (j, "rotation", e.transform.rotation);
    readVec3  (j, "scale",    e.transform.scale);
    if (j.contains("mesh"))     e.mesh     = meshFromJson(j["mesh"]);
    if (j.contains("material")) e.material = materialFromJson(j["material"]);
    return e;
}

}  // namespace

bool saveSceneFile(const SceneFile& scene, const std::string& path) {
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
    for (const auto& e : scene.entities) ents.push_back(entityToJson(e));
    root["entities"] = ents;

    std::ofstream f(path);
    if (!f) {
        Log::error("SceneIO: cannot open '%s' for writing", path.c_str());
        return false;
    }
    f << root.dump(2);
    return true;
}

std::optional<SceneFile> loadSceneFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        Log::error("SceneIO: cannot open '%s'", path.c_str());
        return std::nullopt;
    }

    json root;
    try {
        f >> root;
    } catch (const json::parse_error& e) {
        Log::error("SceneIO: parse error in '%s': %s", path.c_str(), e.what());
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
            scene.entities.push_back(entityFromJson(j));
        }
    }
    return scene;
}

}  // namespace iron
