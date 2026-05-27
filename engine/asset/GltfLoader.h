#pragma once

#include "scene/Mesh.h"

#include <optional>
#include <string>

namespace iron {

// Loads the first primitive of the first mesh of the first node of the
// default scene from a glTF or GLB file. Returns std::nullopt on parse
// failure, missing required attributes (POSITION, NORMAL, or indices),
// or unsupported accessor types.
//
// `path` can be .gltf (with adjacent .bin) or .glb (single binary file).
// tinygltf auto-detects from the extension.
//
// Attribute mapping (glTF -> engine Vertex):
//   POSITION   (vec3 float)  -> Vertex::position    [required]
//   NORMAL     (vec3 float)  -> Vertex::normal      [required]
//   TEXCOORD_0 (vec2 float)  -> Vertex::uv          [optional; defaults to (0,0)]
//   TANGENT    (vec4 float)  -> Vertex::tangent     [optional; xyz only, w sign dropped;
//                                                   defaults to (1,0,0)]
//   indices    (u16 or u32)  -> u32 indices         [required; u16 promoted to u32]
//
// No texture / material / skin / animation data is loaded -- M22 scope
// is geometry only. Game code passes engine default textures
// (whiteTexture / flatNormalTexture / noSpecularTexture).
std::optional<MeshData> loadGltfMesh(const std::string& path);

}  // namespace iron
