#pragma once

#include "scene/SceneFormat.h"

#include <optional>
#include <string>

namespace iron {

// Load a scene from a JSON file. Returns nullopt on a missing file or
// malformed JSON (logs the error via Log::error). Missing optional fields
// fall back to the SceneFile / SceneEntity struct defaults.
std::optional<SceneFile> loadSceneFile(const std::string& path);

// Write a scene to a JSON file (pretty-printed, 2-space indent, for
// human-diffable output). Returns false if the file can't be opened.
bool saveSceneFile(const SceneFile& scene, const std::string& path);

}  // namespace iron
