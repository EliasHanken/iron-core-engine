#pragma once

#include "math/Vec.h"
#include "scene/SceneFormat.h"

#include <nlohmann/json.hpp>

#include <string>

namespace iron {

class Reflection;
class ComponentRegistry;

// Shared scene/prefab JSON helpers. entityToJson / entityFromJson are the single
// source of truth for an entity's on-disk schema (name, transform, mesh,
// material, parentIndex, components) — both SceneIO and PrefabIO use them so the
// two formats can never drift apart.

nlohmann::json vec3ToJson(const Vec3& v);
void readVec3(const nlohmann::json& j, const char* key, Vec3& out);
void readFloat(const nlohmann::json& j, const char* key, float& out);
void readString(const nlohmann::json& j, const char* key, std::string& out);

nlohmann::json entityToJson(const Reflection& r, const ComponentRegistry& cr,
                            const SceneEntity& e);
SceneEntity    entityFromJson(const Reflection& r, const ComponentRegistry& cr,
                              const nlohmann::json& j);

}  // namespace iron
