#include "scene/PrefabIO.h"

#include "core/Log.h"
#include "scene/EntityJson.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>

namespace iron {

namespace {
using json = nlohmann::json;

// Reset parentIndex links that are out of range, self-referential, or cyclic
// (mirrors SceneIO's load-time posture). The root (index 0) is always forced to
// -1.
void sanitizeParents(Prefab& p) {
    const int n = static_cast<int>(p.entities.size());
    if (n == 0) return;
    if (p.entities[0].parentIndex != -1) {          // root is the root by definition
        Log::warn("PrefabIO: entity 0 has non-root parent %d; forcing to -1",
                  p.entities[0].parentIndex);
        p.entities[0].parentIndex = -1;
    }
    for (int i = 1; i < n; ++i) {
        int& pi = p.entities[i].parentIndex;
        if (pi < -1 || pi >= n || pi == i) {
            Log::warn("PrefabIO: entity %d has invalid parent %d; resetting to root", i, pi);
            pi = -1;
        }
    }
    for (int i = 1; i < n; ++i) {
        int hops = 0, pi = p.entities[i].parentIndex;
        while (pi != -1) {
            if (++hops > n) {
                Log::warn("PrefabIO: entity %d parent chain cycles; resetting to root", i);
                p.entities[i].parentIndex = -1;
                break;
            }
            pi = p.entities[pi].parentIndex;
        }
    }
}
}  // namespace

std::string prefabToJsonString(const Reflection& r, const ComponentRegistry& cr,
                               const Prefab& prefab) {
    json root = json::object();
    root["version"] = prefab.version;
    json ents = json::array();
    for (const auto& e : prefab.entities) ents.push_back(entityToJson(r, cr, e));
    root["entities"] = std::move(ents);
    return root.dump();
}

std::optional<Prefab> prefabFromJsonString(const Reflection& r, const ComponentRegistry& cr,
                                           const std::string& jsonStr) {
    json root;
    try {
        root = json::parse(jsonStr);
    } catch (const json::exception& e) {
        Log::error("PrefabIO: parse error: %s", e.what());
        return std::nullopt;
    }
    if (!root.contains("entities") || !root["entities"].is_array() ||
        root["entities"].empty()) {
        Log::error("PrefabIO: missing or empty 'entities' array");
        return std::nullopt;
    }

    Prefab p;
    if (root.contains("version") && root["version"].is_number_integer())
        p.version = root["version"].get<int>();
    for (const auto& j : root["entities"])
        p.entities.push_back(entityFromJson(r, cr, j));

    sanitizeParents(p);
    return p;
}

bool savePrefabFile(const Reflection& r, const ComponentRegistry& cr,
                    const Prefab& prefab, const std::string& path) {
    std::ofstream f(path);
    if (!f) {
        Log::error("PrefabIO: cannot open '%s' for writing", path.c_str());
        return false;
    }
    json root = json::parse(prefabToJsonString(r, cr, prefab));
    f << root.dump(2);
    return true;
}

std::optional<Prefab> loadPrefabFile(const Reflection& r, const ComponentRegistry& cr,
                                     const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        Log::error("PrefabIO: cannot open '%s'", path.c_str());
        return std::nullopt;
    }
    std::string contents((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return prefabFromJsonString(r, cr, contents);
}

}  // namespace iron
