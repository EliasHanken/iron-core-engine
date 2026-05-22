#pragma once

#include "math/Aabb.h"
#include "math/Mat4.h"
#include "math/Ray.h"
#include "math/Vec.h"
#include "physics/Rope.h"
#include "render/Renderer.h"

#include <vector>

// The Strandbound rope tool: the player places anchor points on world
// surfaces and ties / cuts ropes between them. Game-specific interaction —
// it lives with the game, not the engine.
class RopeTool {
public:
    // `colliders` are the static world boxes the aim ray is tested against
    // when placing an anchor. `renderer` is used to create the rope's mesh and
    // texture; `litShader` is the shader the rope is drawn with.
    RopeTool(std::vector<iron::Aabb> colliders, iron::Renderer& renderer,
             iron::ShaderHandle litShader);

    // Advance one fixed step. `aim` is the player's aim ray; `playerPos` is
    // the player's feet position. The three flags are this step's input edges.
    void update(const iron::Ray& aim, iron::Vec3 playerPos,
                bool placePressed, bool tiePressed, bool cutPressed,
                float dt);

    // Rebuild and draw the rope mesh, and queue the anchor and aim markers as
    // debug lines. Call between submitting the scene and flushDebugLines.
    void draw(iron::Renderer& renderer, const iron::Mat4& view,
              const iron::Mat4& projection) const;

    // Number of placed anchors / live ropes — for HUD readouts.
    int anchorCount() const { return static_cast<int>(anchors_.size()); }
    int ropeCount() const { return static_cast<int>(ropes_.size()); }

private:
    enum class AimKind { None, Surface, Anchor, Rope };

    int pickAnchor(const iron::Ray& aim) const;
    int pickRope(const iron::Ray& aim, iron::Vec3& outPoint) const;
    bool pickSurface(const iron::Ray& aim, iron::Vec3& outPoint) const;
    void refreshAimTarget(const iron::Ray& aim);

    std::vector<iron::Aabb> colliders_;
    std::vector<iron::Vec3> anchors_;
    std::vector<iron::Rope> ropes_;

    int tyingFromAnchor_ = -1;        // anchor index being tied from, or -1
    iron::Vec3 playerPos_{};          // cached, for drawing the tying guide

    AimKind aimKind_ = AimKind::None;
    iron::Vec3 aimPoint_{};

    iron::ShaderHandle litShader_ = iron::kInvalidHandle;
    iron::TextureHandle ropeTexture_ = iron::kInvalidHandle;
    iron::MeshHandle ropesMesh_ = iron::kInvalidHandle;
};
