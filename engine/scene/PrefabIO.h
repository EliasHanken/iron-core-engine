#pragma once

#include "scene/Prefab.h"

#include <optional>
#include <string>

namespace iron {

class Reflection;
class ComponentRegistry;

// Serialize a prefab to a JSON string: {"version":1,"entities":[...]} using the
// shared entity schema (EntityJson). Compact (no pretty-print).
std::string prefabToJsonString(const Reflection& r, const ComponentRegistry& cr,
                               const Prefab& prefab);

// Parse a prefab from a JSON string. Returns nullopt on malformed JSON, a
// missing/empty "entities" array, or a non-array "entities" (logs via
// Log::error). parentIndex values are sanitized exactly like SceneIO
// (out-of-range / self-parent / cycle -> reset to -1 with a warning); the root
// (entities[0]) is forced to parentIndex -1.
std::optional<Prefab> prefabFromJsonString(const Reflection& r, const ComponentRegistry& cr,
                                           const std::string& json);

// Write a prefab to a file (pretty-printed, 2-space). Returns false if the file
// can't be opened (logs via Log::error).
bool savePrefabFile(const Reflection& r, const ComponentRegistry& cr,
                    const Prefab& prefab, const std::string& path);

// Load a prefab from a file. nullopt on a missing file or any error from
// prefabFromJsonString.
std::optional<Prefab> loadPrefabFile(const Reflection& r, const ComponentRegistry& cr,
                                     const std::string& path);

}  // namespace iron
