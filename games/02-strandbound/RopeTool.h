#pragma once

#include "math/Mat4.h"
#include "math/Ray.h"
#include "math/Vec.h"
#include "physics/Rope.h"
#include "render/Renderer.h"

#include <vector>

// The Strandbound rope collection: it owns the tied ropes, draws them, holds a
// finite pool of ropes the player can still deploy, and handles cutting. Ropes
// are created by RopeThrower (a thrown rope that landed) via addRope.
// Game-specific — it lives with the game, not the engine.
class RopeTool {
public:
    RopeTool(iron::Renderer& renderer, iron::ShaderHandle litShader);

    // Adds a rope between two world points if the pool is non-empty: builds a
    // slack Rope, spends one from the pool, and returns true. Returns false
    // and adds nothing when the pool is empty.
    bool addRope(iron::Vec3 nearEnd, iron::Vec3 farEnd);

    // Advance one fixed step: if `cutPressed`, cut the rope under `aim` (which
    // refunds it to the pool); then step every rope's physics.
    void update(const iron::Ray& aim, bool cutPressed, float dt);

    // Rebuild and draw the rope tube mesh, and queue an endpoint marker at
    // each rope end as debug lines. Call between submitting the scene and
    // flushDebugLines.
    void draw(iron::Renderer& renderer, const iron::Mat4& view,
              const iron::Mat4& projection) const;

    // Ropes the player can still deploy — for the HUD readout.
    int ropesAvailable() const { return ropesAvailable_; }

    // The live ropes — for RopeWalker to read endpoints and points.
    const std::vector<iron::Rope>& ropes() const { return ropes_; }

private:
    // Index of the rope whose points the aim ray passes nearest, or -1.
    int pickRope(const iron::Ray& aim) const;

    std::vector<iron::Rope> ropes_;
    // Invariant: ropesAvailable_ + ropes_.size() == kStartingRopes — addRope
    // spends one, a cut refunds one.
    int ropesAvailable_;

    iron::ShaderHandle litShader_ = iron::kInvalidHandle;
    iron::TextureHandle ropeTexture_ = iron::kInvalidHandle;
    iron::MeshHandle ropesMesh_ = iron::kInvalidHandle;
};
