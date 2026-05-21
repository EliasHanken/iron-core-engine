#pragma once

#include "math/Aabb.h"
#include "math/Ray.h"
#include "math/Vec.h"
#include "physics/Rope.h"

#include <vector>

namespace iron { class Renderer; }

// The Strandbound rope tool: the player places anchor points on world
// surfaces and ties / cuts ropes between them. This is game-specific
// interaction logic, so it lives with the game rather than the engine.
class RopeTool {
public:
    // `colliders` are the static world boxes (islands, props, pole) the
    // aim ray is tested against when placing an anchor.
    explicit RopeTool(std::vector<iron::Aabb> colliders);

    // Advance one fixed step. `aim` is the player's aim ray; `playerPos` is
    // the player's feet position. The three flags are this step's input
    // edges (true only on the step the button/key went down).
    void update(const iron::Ray& aim, iron::Vec3 playerPos,
                bool placePressed, bool tiePressed, bool cutPressed,
                float dt);

    // Queue debug lines for anchors, ropes, the tying guide line, and the
    // aim marker. Call between submitting the scene and flushDebugLines.
    void draw(iron::Renderer& renderer) const;

private:
    enum class AimKind { None, Surface, Anchor, Rope };

    // Nearest anchor the aim ray passes through, or -1.
    int pickAnchor(const iron::Ray& aim) const;
    // Nearest rope the aim ray passes through, or -1; sets outPoint to the
    // rope point that was hit.
    int pickRope(const iron::Ray& aim, iron::Vec3& outPoint) const;
    // Nearest surface hit of the aim ray against the colliders.
    bool pickSurface(const iron::Ray& aim, iron::Vec3& outPoint) const;
    // Recompute what the aim ray currently targets (for the aim marker).
    void refreshAimTarget(const iron::Ray& aim);

    std::vector<iron::Aabb> colliders_;
    std::vector<iron::Vec3> anchors_;
    std::vector<iron::Rope> ropes_;

    int tyingFromAnchor_ = -1;        // anchor index being tied from, or -1
    iron::Vec3 playerPos_{};          // cached, for drawing the tying guide

    AimKind aimKind_ = AimKind::None;
    iron::Vec3 aimPoint_{};
};
