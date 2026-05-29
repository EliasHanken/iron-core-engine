#pragma once

#include "math/Aabb.h"
#include "math/Mat4.h"
#include "math/Ray.h"
#include "math/Vec.h"

#include <vector>

namespace iron {

// Unproject a mouse pixel to a world-space pick ray. `mousePx` is in pixels with
// the origin at the top-left (GLFW convention); `viewportPx` is the framebuffer
// size; `camPos` is the camera world position (the ray origin). Reconstructs the
// far-plane world point via inverse(proj*view) and points the ray from camPos
// through it.
Ray screenPointToRay(const Mat4& view, const Mat4& proj, Vec2 mousePx,
                     Vec2 viewportPx, Vec3 camPos);

// Returns the index of the nearest AABB the ray hits (smallest entry distance),
// or -1 if the ray misses them all.
int pickEntity(const Ray& ray, const std::vector<Aabb>& worldAabbs);

}  // namespace iron
