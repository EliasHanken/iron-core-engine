#include "scene/SceneIO.h"

#include "core/Log.h"
#include "scene/ReflectionIO.h"

#include <nlohmann/json.hpp>

#include <fstream>

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

json entityToJson(const Reflection& r, const SceneEntity& e) {
    json j = json::object();
    j["name"]      = e.name;
    j["transform"] = componentToJson(r, e.transform);
    j["mesh"]      = componentToJson(r, e.mesh);
    j["material"]  = componentToJson(r, e.material);
    if (e.collision) j["collision"] = componentToJson(r, *e.collision);
    if (e.audio)     j["audio"]     = componentToJson(r, *e.audio);
    if (e.probe)     j["probe"]     = componentToJson(r, *e.probe);
    return j;
}

SceneEntity entityFromJson(const Reflection& r, const json& j) {
    SceneEntity e;
    readString(j, "name", e.name);
    if (j.contains("transform")) componentFromJson(r, e.transform, j["transform"]);
    if (j.contains("mesh"))      componentFromJson(r, e.mesh,      j["mesh"]);
    if (j.contains("material"))  componentFromJson(r, e.material,  j["material"]);
    if (j.contains("collision")) {
        e.collision = CollisionShape{};
        componentFromJson(r, *e.collision, j["collision"]);
    }
    if (j.contains("audio")) {
        e.audio = AudioEmitter{};
        componentFromJson(r, *e.audio, j["audio"]);
    }
    if (j.contains("probe")) {
        e.probe = ReflectionProbeDef{};
        componentFromJson(r, *e.probe, j["probe"]);
    }
    return e;
}

}  // namespace

bool saveSceneFile(const Reflection& reflection,
                   const SceneFile& scene,
                   const std::string& path) {
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
    for (const auto& e : scene.entities) ents.push_back(entityToJson(reflection, e));
    root["entities"] = ents;

    std::ofstream f(path);
    if (!f) {
        Log::error("SceneIO: cannot open '%s' for writing", path.c_str());
        return false;
    }
    f << root.dump(2);
    return true;
}

std::optional<SceneFile> loadSceneFile(const Reflection& reflection,
                                       const std::string& path) {
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
            scene.entities.push_back(entityFromJson(reflection, j));
        }
    }
    return scene;
}

}  // namespace iron
