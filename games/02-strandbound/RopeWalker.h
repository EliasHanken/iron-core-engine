#pragma once

#include "math/Aabb.h"
#include "math/Mat4.h"
#include "math/Vec.h"
#include "physics/Rope.h"

#include <vector>

// Strandbound's tightrope traversal. Game-specific gameplay — it lives with
// the game, not the engine. Nothing here touches OpenGL, so it is headless
// and unit-testable.

// --- Pure helpers ---

// True if the world point (x, z) lies within the XZ footprint of any AABB in
// `islands` — i.e. the player has solid ground beneath them.
bool hasFooting(float x, float z, const std::vector<iron::Aabb>& islands);

// The lean-perturbation magnitude (per second). Grows with `timeOnRope`
// (seconds) so a long crossing gets harder; capped so it stays fair.
float leanDriftMagnitude(float timeOnRope);

// The new lean after one step. Lean is unstable — displacement self-amplifies,
// like an inverted pendulum — so the player must actively counter-steer.
// `nudge` is a small random perturbation; `steer` (-1..+1) is the player's
// correction input. Not clamped: the caller tests |lean| >= 1 for a fall.
float applyLean(float lean, float nudge, float steer, float dt);

// Advances a traversal parameter `t` in [0, 1] by `input` (-1..+1) at a walk
// speed of `walkSpeed` units/second along a rope of length `ropeLength`,
// clamped to [0, 1]. A non-positive `ropeLength` leaves `t` unchanged.
float advanceParam(float t, float input, float walkSpeed, float ropeLength,
                   float dt);

// Index of the first rope in `ropes` whose start or end point is within
// `radius` (measured horizontally, in the XZ plane) of `playerFeet`, or -1 if
// none. On a hit, `outAtStart` is set true if the near end is the rope's first
// point, false if it is the last.
int findMountRope(iron::Vec3 playerFeet, const std::vector<iron::Rope>& ropes,
                  float radius, bool& outAtStart);
