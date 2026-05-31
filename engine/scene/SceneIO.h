#pragma once

#include "scene/SceneFormat.h"

#include <optional>
#include <string>

namespace iron {

class Reflection;

// Load a scene from a JSON file. Returns nullopt on a missing file or
// malformed JSON (logs the error via Log::error). Missing optional fields
// fall back to the SceneFile / SceneEntity struct defaults. The Reflection
// registry is consulted for component (transform / mesh / material) fields
// and any enum registered in it.
std::optional<SceneFile> loadSceneFile(const Reflection& reflection,
                                       const std::string& path);

// Write a scene to a JSON file (pretty-printed, 2-space indent, for
// human-diffable output). Returns false if the file can't be opened.
bool saveSceneFile(const Reflection& reflection,
                   const SceneFile& scene,
                   const std::string& path);

}  // namespace iron
