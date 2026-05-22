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

// An active tightrope traversal. Constructed once and reused: begin() starts a
// crossing, step() advances it. Holds no reference to the rope — the rope is
// re-supplied each step.
class RopeWalker {
public:
    // The result of a step (or the state begin() leaves the walker in).
    enum class Result { Traversing, Dismounted, Fell, Won };

    // Start traversing `rope`. `atStart` true means the player mounted at the
    // rope's first point (t=0 there, t=1 at the last point); false means the
    // last point (the t axis is reversed). `yaw`/`pitch` seed the look
    // direction from the player's current facing.
    void begin(const iron::Rope& rope, bool atStart, float yaw, float pitch);

    // Advance one fixed step. `forward` (-1..+1) moves along the rope, `steer`
    // (-1..+1) counter-steers the lean, `mouseDX`/`mouseDY` turn the look,
    // `driftRandom` (-1..+1) is this step's random lean perturbation.
    // `rope` is the rope being traversed; `farIsland` holds the AABB(s) that
    // count as the winning island.
    Result step(float forward, float steer, float mouseDX, float mouseDY,
                float driftRandom, float dt, const iron::Rope& rope,
                const std::vector<iron::Aabb>& farIsland);

    float lean() const { return lean_; }  // |lean| < 1 while traversing; HUD meter
    iron::Mat4 viewMatrix() const;                   // camera, including roll
    iron::Vec3 exitFeet() const { return exitFeet_; }  // where to drop the player

private:
    // The interpolated rope point at the current t_ (no eye-height offset).
    iron::Vec3 sampleRope(const iron::Rope& rope) const;
    // The rope's mounted-end / far-end point, accounting for atStart_.
    iron::Vec3 mountEndPoint(const iron::Rope& rope) const;
    iron::Vec3 farEndPoint(const iron::Rope& rope) const;

    float t_ = 0.0f;
    bool atStart_ = true;
    float lean_ = 0.0f;
    float timeOnRope_ = 0.0f;
    float ropeLength_ = 1.0f;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    iron::Vec3 eye_{};
    iron::Vec3 exitFeet_{};
};
