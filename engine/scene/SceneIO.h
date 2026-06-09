#pragma once

#include "scene/SceneFormat.h"

#include <optional>
#include <string>

namespace iron {

class Reflection;
class ComponentRegistry;

// Load a scene from a JSON file. Returns nullopt on a missing file or
// malformed JSON (logs the error via Log::error). Missing optional fields
// fall back to the SceneFile / SceneEntity struct defaults. The Reflection
// registry is consulted for component (transform / mesh / material) fields
// and any enum registered in it.
std::optional<SceneFile> loadSceneFile(const Reflection& reflection,
                                       const ComponentRegistry& cr,
                                       const std::string& path);

// Write a scene to a JSON file (pretty-printed, 2-space indent, for
// human-diffable output). Returns false if the file can't be opened.
bool saveSceneFile(const Reflection& reflection,
                   const ComponentRegistry& cr,
                   const SceneFile& scene,
                   const std::string& path);

// Serialize a scene to a JSON string (same schema as saveSceneFile, compact —
// no pretty-print, since this feeds the undo stack, not a human-diffable file).
std::string sceneToJsonString(const Reflection& reflection,
                              const ComponentRegistry& cr,
                              const SceneFile& scene);

// Parse a scene from a JSON string. Returns nullopt on malformed JSON (logs via
// Log::error). Missing optional fields fall back to struct defaults — identical
// semantics to loadSceneFile, minus the file I/O.
std::optional<SceneFile> sceneFromJsonString(const Reflection& reflection,
                                             const ComponentRegistry& cr,
                                             const std::string& json);

}  // namespace iron
