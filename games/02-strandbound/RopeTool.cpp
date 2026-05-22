#include "RopeTool.h"

#include "core/Platform.h"
#include "physics/VerletPoint.h"
#include "scene/Mesh.h"

#include <cstddef>
#include <vector>

namespace {
constexpr int kRopeSegments = 20;
constexpr float kSlackFactor = 1.35f;     // rope length vs. endpoint span
constexpr float kRopePickRadius = 0.3f;   // rope points picked as spheres
constexpr float kMarkerSize = 0.25f;      // endpoint marker cross half-length
constexpr float kRopeRadius = 0.055f;     // visual rope thickness
constexpr int kRopeSides = 6;             // low-poly tube cross-section
constexpr int kStartingRopes = 5;         // ropes the player starts with
}  // namespace

RopeTool::RopeTool(iron::Renderer& renderer, iron::ShaderHandle litShader)
    : ropesAvailable_(kStartingRopes), litShader_(litShader) {
    // The rope texture ships next to the executable. The rope mesh is created
    // empty and refreshed every frame in draw().
    ropeTexture_ =
        renderer.loadTexture(iron::executableDir() + "/assets/rope.jpg");
    ropesMesh_ = renderer.createMesh(iron::MeshData{});
}

bool RopeTool::addRope(iron::Vec3 nearEnd, iron::Vec3 farEnd) {
    if (ropesAvailable_ <= 0) {
        return false;
    }
    const float span = iron::length(farEnd - nearEnd);
    ropes_.push_back(
        iron::Rope(nearEnd, farEnd, kRopeSegments, span * kSlackFactor));
    --ropesAvailable_;
    return true;
}

int RopeTool::pickRope(const iron::Ray& aim) const {
    int best = -1;
    float bestT = 1e30f;
    for (std::size_t i = 0; i < ropes_.size(); ++i) {
        for (const iron::VerletPoint& p : ropes_[i].points()) {
            float t = 0.0f;
            if (iron::intersectRaySphere(aim, p.position, kRopePickRadius, t)
                    && t < bestT) {
                bestT = t;
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}

void RopeTool::update(const iron::Ray& aim, bool cutPressed, float dt) {
    if (cutPressed) {
        const int rope = pickRope(aim);
        if (rope >= 0) {
            ropes_.erase(ropes_.begin() +
                         static_cast<std::ptrdiff_t>(rope));
            ++ropesAvailable_;  // a cut rope is recovered to the pool
        }
    }
    for (iron::Rope& r : ropes_) {
        r.update(dt);
    }
}

void RopeTool::draw(iron::Renderer& renderer) const {
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

    iron::DrawCall ropeCall;
    ropeCall.mesh = ropesMesh_;
    ropeCall.shader = litShader_;
    ropeCall.texture = ropeTexture_;
    renderer.submit(ropeCall);

    // A small yellow cross at each rope's two endpoints — the mount points.
    const iron::Vec3 markerColor{0.95f, 0.8f, 0.2f};
    const float s = kMarkerSize;
    for (const iron::Rope& r : ropes_) {
        if (r.points().size() < 2) {
            continue;
        }
        const iron::Vec3 ends[2] = {r.points().front().position,
                                    r.points().back().position};
        for (const iron::Vec3& e : ends) {
            renderer.drawLine(e - iron::Vec3{s, 0.0f, 0.0f},
                              e + iron::Vec3{s, 0.0f, 0.0f}, markerColor);
            renderer.drawLine(e - iron::Vec3{0.0f, s, 0.0f},
                              e + iron::Vec3{0.0f, s, 0.0f}, markerColor);
            renderer.drawLine(e - iron::Vec3{0.0f, 0.0f, s},
                              e + iron::Vec3{0.0f, 0.0f, s}, markerColor);
        }
    }
}
