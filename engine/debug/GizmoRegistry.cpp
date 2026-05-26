#include "debug/GizmoRegistry.h"

#include <cmath>

namespace {

// 12 edges of an AABB defined by min/max corners. Each edge is two Vec3
// offsets in [0,1] that we lerp between min and max to get the endpoints.
struct EdgeIdx { int aBits; int bBits; };  // bits: x=0b001, y=0b010, z=0b100
constexpr EdgeIdx kAabbEdges[12] = {
    // bottom rectangle (y=0)
    {0b000, 0b001}, {0b001, 0b101}, {0b101, 0b100}, {0b100, 0b000},
    // top rectangle (y=1)
    {0b010, 0b011}, {0b011, 0b111}, {0b111, 0b110}, {0b110, 0b010},
    // verticals
    {0b000, 0b010}, {0b001, 0b011}, {0b101, 0b111}, {0b100, 0b110},
};

iron::Vec3 cornerOf(int bits, iron::Vec3 minP, iron::Vec3 maxP) {
    return iron::Vec3{
        (bits & 0b001) ? maxP.x : minP.x,
        (bits & 0b010) ? maxP.y : minP.y,
        (bits & 0b100) ? maxP.z : minP.z,
    };
}

constexpr int kSphereSegments = 32;

}  // namespace

namespace iron {

GizmoRegistry::GizmoRegistry() = default;

std::uint16_t GizmoRegistry::categoryIdFor(std::string_view name) {
    auto it = categoryToId_.find(std::string(name));
    if (it != categoryToId_.end()) return it->second;
    const std::uint16_t id = static_cast<std::uint16_t>(categoryToId_.size());
    categoryToId_.emplace(std::string(name), id);
    categoryEnabled_.push_back(true);
    return id;
}

void GizmoRegistry::enable(std::string_view category, bool on) {
    const std::uint16_t id = categoryIdFor(category);
    categoryEnabled_[id] = on;
}

bool GizmoRegistry::isEnabled(std::string_view category) const {
    auto it = categoryToId_.find(std::string(category));
    if (it == categoryToId_.end()) return true;  // unknown = default-on
    return categoryEnabled_[it->second];
}

void GizmoRegistry::enableAll(bool on) {
    masterEnabled_ = on;
}

GizmoId GizmoRegistry::addLine(std::string_view category, Vec3 a, Vec3 b,
                               Vec3 color, float lifetimeSec) {
    Entry e;
    e.categoryId = categoryIdFor(category);
    e.kind = Kind::Line;
    e.a = a;
    e.b = b;
    e.color = color;
    e.lifetimeRemaining = (lifetimeSec > 0.0f) ? lifetimeSec : -1.0f;
    if (nextId_ == 0) nextId_ = 1;  // skip kInvalidGizmo on wrap
    const GizmoId id = nextId_++;
    entries_.emplace(id, e);
    return id;
}

GizmoId GizmoRegistry::addAabb(std::string_view category, Vec3 minP, Vec3 maxP,
                               Vec3 color, float lifetimeSec) {
    Entry e;
    e.categoryId = categoryIdFor(category);
    e.kind = Kind::Aabb;
    e.a = minP;
    e.b = maxP;
    e.color = color;
    e.lifetimeRemaining = (lifetimeSec > 0.0f) ? lifetimeSec : -1.0f;
    if (nextId_ == 0) nextId_ = 1;  // skip kInvalidGizmo on wrap
    const GizmoId id = nextId_++;
    entries_.emplace(id, e);
    return id;
}

GizmoId GizmoRegistry::addSphere(std::string_view category, Vec3 center,
                                 float radius, Vec3 color, float lifetimeSec) {
    Entry e;
    e.categoryId = categoryIdFor(category);
    e.kind = Kind::Sphere;
    e.a = center;
    e.radius = radius;
    e.color = color;
    e.lifetimeRemaining = (lifetimeSec > 0.0f) ? lifetimeSec : -1.0f;
    if (nextId_ == 0) nextId_ = 1;  // skip kInvalidGizmo on wrap
    const GizmoId id = nextId_++;
    entries_.emplace(id, e);
    return id;
}

void GizmoRegistry::updateLine(GizmoId id, Vec3 a, Vec3 b, Vec3 color) {
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.kind != Kind::Line) return;
    it->second.a = a;
    it->second.b = b;
    it->second.color = color;
}

void GizmoRegistry::updateAabb(GizmoId id, Vec3 minP, Vec3 maxP, Vec3 color) {
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.kind != Kind::Aabb) return;
    it->second.a = minP;
    it->second.b = maxP;
    it->second.color = color;
}

void GizmoRegistry::updateSphere(GizmoId id, Vec3 center, float radius, Vec3 color) {
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.kind != Kind::Sphere) return;
    it->second.a = center;
    it->second.radius = radius;
    it->second.color = color;
}

void GizmoRegistry::remove(GizmoId id) {
    entries_.erase(id);
}

void GizmoRegistry::clearCategory(std::string_view category) {
    auto cit = categoryToId_.find(std::string(category));
    if (cit == categoryToId_.end()) return;
    const std::uint16_t cid = cit->second;
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (it->second.categoryId == cid) it = entries_.erase(it);
        else ++it;
    }
}

void GizmoRegistry::clearAll() {
    entries_.clear();
}

void GizmoRegistry::tick(float dt, Renderer& renderer) {
    // Advance expiries; remove anything that's now < 0 if it was a
    // finite-lifetime entry.
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        Entry& e = it->second;
        if (e.lifetimeRemaining >= 0.0f) {
            e.lifetimeRemaining -= dt;
            if (e.lifetimeRemaining < 0.0f) {
                it = entries_.erase(it);
                continue;
            }
        }
        ++it;
    }

    if (!masterEnabled_) return;
    for (const auto& [id, e] : entries_) {
        if (e.categoryId < categoryEnabled_.size() &&
            !categoryEnabled_[e.categoryId]) continue;
        switch (e.kind) {
            case Kind::Line:
                renderer.drawLine(e.a, e.b, e.color);
                break;
            case Kind::Aabb: {
                for (const EdgeIdx& edge : kAabbEdges) {
                    const Vec3 ea = cornerOf(edge.aBits, e.a, e.b);
                    const Vec3 eb = cornerOf(edge.bBits, e.a, e.b);
                    renderer.drawLine(ea, eb, e.color);
                }
                break;
            }
            case Kind::Sphere: {
                // 3 great-circle loops in the XY, YZ, XZ planes.
                constexpr float kTwoPi = 6.28318530718f;
                for (int axis = 0; axis < 3; ++axis) {
                    Vec3 prev{0, 0, 0};
                    for (int i = 0; i <= kSphereSegments; ++i) {
                        const float t = (static_cast<float>(i) / kSphereSegments) * kTwoPi;
                        const float c = std::cos(t) * e.radius;
                        const float s = std::sin(t) * e.radius;
                        Vec3 p = e.a;
                        if (axis == 0)      { p.x += c; p.y += s; }
                        else if (axis == 1) { p.y += c; p.z += s; }
                        else                { p.x += c; p.z += s; }
                        if (i > 0) renderer.drawLine(prev, p, e.color);
                        prev = p;
                    }
                }
                break;
            }
        }
    }
}

}  // namespace iron
