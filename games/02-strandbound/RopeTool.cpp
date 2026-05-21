#include "RopeTool.h"

#include "physics/VerletPoint.h"
#include "render/Renderer.h"

#include <cstddef>
#include <utility>

namespace {
constexpr float kAnchorPickRadius = 0.5f;   // anchors picked as spheres
constexpr float kRopePickRadius = 0.3f;     // rope points picked as spheres
constexpr int kRopeSegments = 20;
constexpr float kSlackFactor = 1.35f;       // rope length vs. anchor span
constexpr float kMarkerSize = 0.25f;
}  // namespace

RopeTool::RopeTool(std::vector<iron::Aabb> colliders)
    : colliders_(std::move(colliders)) {}

int RopeTool::pickAnchor(const iron::Ray& aim) const {
    int best = -1;
    float bestT = 1e30f;
    for (std::size_t i = 0; i < anchors_.size(); ++i) {
        float t = 0.0f;
        if (iron::intersectRaySphere(aim, anchors_[i], kAnchorPickRadius, t)
                && t < bestT) {
            bestT = t;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int RopeTool::pickRope(const iron::Ray& aim, iron::Vec3& outPoint) const {
    int best = -1;
    float bestT = 1e30f;
    for (std::size_t i = 0; i < ropes_.size(); ++i) {
        for (const iron::VerletPoint& p : ropes_[i].points()) {
            float t = 0.0f;
            if (iron::intersectRaySphere(aim, p.position, kRopePickRadius, t)
                    && t < bestT) {
                bestT = t;
                best = static_cast<int>(i);
                outPoint = p.position;
            }
        }
    }
    return best;
}

bool RopeTool::pickSurface(const iron::Ray& aim, iron::Vec3& outPoint) const {
    float bestT = 1e30f;
    bool found = false;
    for (const iron::Aabb& box : colliders_) {
        float t = 0.0f;
        if (iron::intersectRayAabb(aim, box, t) && t < bestT) {
            bestT = t;
            found = true;
        }
    }
    if (found) {
        outPoint = aim.origin + aim.direction * bestT;
    }
    return found;
}

void RopeTool::refreshAimTarget(const iron::Ray& aim) {
    const int anchor = pickAnchor(aim);
    if (anchor >= 0) {
        aimKind_ = AimKind::Anchor;
        aimPoint_ = anchors_[static_cast<std::size_t>(anchor)];
        return;
    }
    iron::Vec3 ropePoint;
    if (pickRope(aim, ropePoint) >= 0) {
        aimKind_ = AimKind::Rope;
        aimPoint_ = ropePoint;
        return;
    }
    iron::Vec3 surfacePoint;
    if (pickSurface(aim, surfacePoint)) {
        aimKind_ = AimKind::Surface;
        aimPoint_ = surfacePoint;
        return;
    }
    aimKind_ = AimKind::None;
}

void RopeTool::update(const iron::Ray& aim, iron::Vec3 playerPos,
                      bool placePressed, bool tiePressed, bool cutPressed,
                      float dt) {
    playerPos_ = playerPos;

    // Place: drop an anchor on the nearest surface the aim ray hits.
    if (placePressed) {
        iron::Vec3 hit;
        if (pickSurface(aim, hit)) {
            anchors_.push_back(hit);
        }
    }

    // Tie: first click picks the start anchor; second click (a different
    // anchor) creates a rope spanning the two.
    if (tiePressed) {
        const int anchor = pickAnchor(aim);
        if (anchor >= 0) {
            if (tyingFromAnchor_ < 0) {
                tyingFromAnchor_ = anchor;
            } else if (anchor != tyingFromAnchor_) {
                const iron::Vec3 a =
                    anchors_[static_cast<std::size_t>(tyingFromAnchor_)];
                const iron::Vec3 b =
                    anchors_[static_cast<std::size_t>(anchor)];
                const float span = iron::length(b - a);
                ropes_.push_back(iron::Rope(a, b, kRopeSegments,
                                            span * kSlackFactor));
                tyingFromAnchor_ = -1;
            }
        }
    }

    // Cut: remove the whole rope the aim ray hits.
    if (cutPressed) {
        iron::Vec3 unused;
        const int rope = pickRope(aim, unused);
        if (rope >= 0) {
            ropes_.erase(ropes_.begin()
                         + static_cast<std::ptrdiff_t>(rope));
        }
    }

    // Advance every rope's Verlet simulation. Each rope was constructed
    // pinned to its two anchor positions and is never re-synced — this relies
    // on anchors being static in M4. Revisit if anchors ever become movable.
    for (iron::Rope& r : ropes_) {
        r.update(dt);
    }

    refreshAimTarget(aim);
}

void RopeTool::draw(iron::Renderer& renderer) const {
    const iron::Vec3 anchorColor{0.95f, 0.8f, 0.2f};
    const iron::Vec3 ropeColor{0.55f, 0.35f, 0.18f};
    const iron::Vec3 guideColor{0.3f, 0.85f, 0.95f};

    // Anchors: a small three-axis cross at each.
    for (const iron::Vec3& a : anchors_) {
        const float s = kMarkerSize;
        renderer.drawLine(a - iron::Vec3{s, 0.0f, 0.0f},
                          a + iron::Vec3{s, 0.0f, 0.0f}, anchorColor);
        renderer.drawLine(a - iron::Vec3{0.0f, s, 0.0f},
                          a + iron::Vec3{0.0f, s, 0.0f}, anchorColor);
        renderer.drawLine(a - iron::Vec3{0.0f, 0.0f, s},
                          a + iron::Vec3{0.0f, 0.0f, s}, anchorColor);
    }

    // Ropes: one debug line per segment.
    for (const iron::Rope& r : ropes_) {
        const std::vector<iron::VerletPoint>& pts = r.points();
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            renderer.drawLine(pts[i].position, pts[i + 1].position, ropeColor);
        }
    }

    // While tying: a guide line from the start anchor to the player.
    if (tyingFromAnchor_ >= 0) {
        renderer.drawLine(anchors_[static_cast<std::size_t>(tyingFromAnchor_)],
                          playerPos_ + iron::Vec3{0.0f, 1.0f, 0.0f},
                          guideColor);
    }

    // Aim marker: a small cross at the targeted point, coloured by kind.
    if (aimKind_ != AimKind::None) {
        iron::Vec3 c{1.0f, 1.0f, 1.0f};  // Surface -> white
        if (aimKind_ == AimKind::Anchor) {
            c = iron::Vec3{0.95f, 0.8f, 0.2f};
        } else if (aimKind_ == AimKind::Rope) {
            c = iron::Vec3{0.95f, 0.25f, 0.2f};
        }
        const float s = kMarkerSize * 0.7f;
        renderer.drawLine(aimPoint_ - iron::Vec3{s, 0.0f, 0.0f},
                          aimPoint_ + iron::Vec3{s, 0.0f, 0.0f}, c);
        renderer.drawLine(aimPoint_ - iron::Vec3{0.0f, s, 0.0f},
                          aimPoint_ + iron::Vec3{0.0f, s, 0.0f}, c);
        renderer.drawLine(aimPoint_ - iron::Vec3{0.0f, 0.0f, s},
                          aimPoint_ + iron::Vec3{0.0f, 0.0f, s}, c);
    }
}
