#include "RopeTool.h"

#include "core/Platform.h"
#include "physics/VerletPoint.h"
#include "scene/Mesh.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace {
constexpr float kAnchorPickRadius = 0.5f;   // anchors picked as spheres
constexpr float kRopePickRadius = 0.3f;     // rope points picked as spheres
constexpr int kRopeSegments = 20;
constexpr float kSlackFactor = 1.35f;       // rope length vs. anchor span
constexpr float kMarkerSize = 0.25f;        // aim-marker cross half-length
constexpr float kMinPlaceDistance = 0.1f;   // ignore surface hits at the eye

constexpr float kRopeRadius = 0.055f;       // visual rope thickness
constexpr int kRopeSides = 6;               // low-poly tube cross-section
}  // namespace

RopeTool::RopeTool(std::vector<iron::Aabb> colliders, iron::Renderer& renderer,
                   iron::ShaderHandle litShader)
    : colliders_(std::move(colliders)), litShader_(litShader) {
    // The rope texture ships next to the executable. The rope mesh is created
    // empty and refreshed every frame in draw(); anchors are drawn as debug
    // lines, so they need no GPU resources of their own.
    ropeTexture_ = renderer.loadTexture(iron::executableDir() + "/assets/rope.jpg");
    ropesMesh_ = renderer.createMesh(iron::MeshData{});
}

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
        // Skip hits at ~t=0: those happen when the player's eye is inside a
        // box, and would place an anchor floating at the eye position.
        if (iron::intersectRayAabb(aim, box, t) && t > kMinPlaceDistance
                && t < bestT) {
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

    if (placePressed) {
        iron::Vec3 hit;
        if (pickSurface(aim, hit)) {
            anchors_.push_back(hit);
        }
    }

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

    if (cutPressed) {
        iron::Vec3 unused;
        const int rope = pickRope(aim, unused);
        if (rope >= 0) {
            ropes_.erase(ropes_.begin()
                         + static_cast<std::ptrdiff_t>(rope));
        }
    }

    // Anchors are static in this milestone — ropes are constructed pinned to
    // their anchor positions and never re-synced.
    for (iron::Rope& r : ropes_) {
        r.update(dt);
    }

    refreshAimTarget(aim);
}

void RopeTool::draw(iron::Renderer& renderer, const iron::Mat4& view,
                    const iron::Mat4& projection) const {
    // Rebuild the combined rope tube mesh from every rope's current points.
    iron::MeshData ropeGeometry;
    for (const iron::Rope& r : ropes_) {
        std::vector<iron::Vec3> pts;
        pts.reserve(r.points().size());
        for (const iron::VerletPoint& p : r.points()) {
            pts.push_back(p.position);
        }
        iron::appendTube(ropeGeometry, pts, kRopeRadius, kRopeSides);
    }
    renderer.updateMesh(ropesMesh_, ropeGeometry);

    // Draw the rope tubes through the lit render path.
    iron::DrawCall ropeCall;
    ropeCall.mesh = ropesMesh_;
    ropeCall.shader = litShader_;
    ropeCall.texture = ropeTexture_;
    renderer.submit(ropeCall, view, projection);

    // Anchors: a small yellow three-axis cross at each (a crisp point marker).
    const iron::Vec3 anchorColor{0.95f, 0.8f, 0.2f};
    for (const iron::Vec3& a : anchors_) {
        const float s = kMarkerSize;
        renderer.drawLine(a - iron::Vec3{s, 0.0f, 0.0f},
                          a + iron::Vec3{s, 0.0f, 0.0f}, anchorColor);
        renderer.drawLine(a - iron::Vec3{0.0f, s, 0.0f},
                          a + iron::Vec3{0.0f, s, 0.0f}, anchorColor);
        renderer.drawLine(a - iron::Vec3{0.0f, 0.0f, s},
                          a + iron::Vec3{0.0f, 0.0f, s}, anchorColor);
    }

    // While tying: a guide line from the start anchor to the player.
    if (tyingFromAnchor_ >= 0) {
        renderer.drawLine(anchors_[static_cast<std::size_t>(tyingFromAnchor_)],
                          playerPos_ + iron::Vec3{0.0f, 1.0f, 0.0f},
                          iron::Vec3{0.3f, 0.85f, 0.95f});
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
