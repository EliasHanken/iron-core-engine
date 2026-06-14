#include "scene/SceneIO.h"

#include "core/Log.h"
#include "scene/EntityJson.h"
#include "world/ComponentRegistry.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>

namespace iron {

using json = nlohmann::json;

std::string sceneToJsonString(const Reflection& reflection,
                              const ComponentRegistry& cr,
                              const SceneFile& scene) {
    json root = json::object();
    root["clearColor"] = vec3ToJson(scene.clearColor);

    json sun = json::object();
    sun["direction"] = vec3ToJson(scene.sun.direction);
    sun["color"]     = vec3ToJson(scene.sun.color);
    sun["ambient"]   = scene.sun.ambient;
    root["sun"] = sun;

    json fog = json::object();
    fog["color"]   = vec3ToJson(scene.fog.color);
    fog["density"] = scene.fog.density;
    root["fog"] = fog;

    json pls = json::array();
    for (const auto& pl : scene.pointLights) {
        json j = json::object();
        j["position"]  = vec3ToJson(pl.position);
        j["color"]     = vec3ToJson(pl.color);
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

    // M69: fail-safe parentIndex validation. Out-of-range, self-parent, or a
    // parent chain that cycles is reset to -1 (root) with a warning — matching
    // SceneIO's load-time posture of never rejecting a file outright.
    const int n = static_cast<int>(scene.entities.size());
    for (int i = 0; i < n; ++i) {
        int p = scene.entities[i].parentIndex;
        if (p < -1 || p >= n || p == i) {   // p < -1 = invalid negative (not the -1 root sentinel)
            Log::warn("SceneIO: entity %d has invalid parent %d; resetting to root", i, p);
            scene.entities[i].parentIndex = -1;
        }
    }
    for (int i = 0; i < n; ++i) {
        int hops = 0, p = scene.entities[i].parentIndex;
        while (p != -1) {
            if (++hops > n) {
                Log::warn("SceneIO: entity %d parent chain cycles; resetting to root", i);
                scene.entities[i].parentIndex = -1;
                break;
            }
            p = scene.entities[p].parentIndex;
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
