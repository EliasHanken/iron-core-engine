#include "debug/GizmoRegistry.h"

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
    const GizmoId id = nextId_++;
    entries_.emplace(id, e);
    return id;
}

GizmoId GizmoRegistry::addAabb(std::string_view, Vec3, Vec3, Vec3, float) {
    return kInvalidGizmo;  // Task 5
}

GizmoId GizmoRegistry::addSphere(std::string_view, Vec3, float, Vec3, float) {
    return kInvalidGizmo;  // Task 5
}

void GizmoRegistry::updateLine(GizmoId id, Vec3 a, Vec3 b, Vec3 color) {
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.kind != Kind::Line) return;
    it->second.a = a;
    it->second.b = b;
    it->second.color = color;
}

void GizmoRegistry::updateAabb(GizmoId, Vec3, Vec3, Vec3) {}
void GizmoRegistry::updateSphere(GizmoId, Vec3, float, Vec3) {}

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

void GizmoRegistry::tick(float, Renderer& renderer) {
    if (!masterEnabled_) return;
    for (const auto& [id, e] : entries_) {
        if (e.categoryId < categoryEnabled_.size() &&
            !categoryEnabled_[e.categoryId]) continue;
        switch (e.kind) {
            case Kind::Line:
                renderer.drawLine(e.a, e.b, e.color);
                break;
            case Kind::Aabb:
            case Kind::Sphere:
                break;  // Task 5
        }
    }
}

}  // namespace iron
