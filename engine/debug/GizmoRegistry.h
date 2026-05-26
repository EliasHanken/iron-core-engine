#pragma once

#include "math/Vec.h"
#include "render/Renderer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iron {

using GizmoId = std::uint32_t;
constexpr GizmoId kInvalidGizmo = 0;

// Retained-mode debug-shape registry. Game code adds named, categorized
// shapes; the registry advances timed-expiry, then emits drawLine into
// the renderer each frame. Backend-agnostic — runs under both OpenGL and
// Vulkan via the abstract Renderer interface.
class GizmoRegistry {
public:
    GizmoRegistry();

    // Category toggles. Disabled categories survive in storage but emit
    // nothing during tick.
    void enable(std::string_view category, bool on);
    bool isEnabled(std::string_view category) const;
    void enableAll(bool on);     // master switch (F3-style)

    // Add. lifetimeSec = 0.0f → persistent until removed.
    GizmoId addLine  (std::string_view category, Vec3 a, Vec3 b,
                      Vec3 color, float lifetimeSec = 0.0f);
    GizmoId addAabb  (std::string_view category, Vec3 minP, Vec3 maxP,
                      Vec3 color, float lifetimeSec = 0.0f);
    GizmoId addSphere(std::string_view category, Vec3 center, float radius,
                      Vec3 color, float lifetimeSec = 0.0f);

    // Update in place. Wrong-kind id is a silent no-op.
    void updateLine  (GizmoId id, Vec3 a, Vec3 b, Vec3 color);
    void updateAabb  (GizmoId id, Vec3 minP, Vec3 maxP, Vec3 color);
    void updateSphere(GizmoId id, Vec3 center, float radius, Vec3 color);

    void remove(GizmoId id);
    void clearCategory(std::string_view category);
    // Removes every entry. Note: category registrations + their enabled
    // flags persist for the registry's lifetime, so dynamic-string
    // category names will accumulate. Use named static categories.
    void clearAll();

    // Per frame: advance expiries, then emit drawLine for everything in
    // enabled categories.
    void tick(float dt, Renderer& renderer);

private:
    enum class Kind : std::uint8_t { Line, Aabb, Sphere };

    struct Entry {
        std::uint16_t categoryId = 0;
        float lifetimeRemaining = -1.0f;  // < 0 = persistent
        Vec3 color{1, 1, 1};
        Kind kind = Kind::Line;
        Vec3 a{0, 0, 0};       // Line.a / Aabb.min / Sphere.center
        Vec3 b{0, 0, 0};       // Line.b / Aabb.max (unused for Sphere)
        float radius = 0.0f;   // Sphere.radius
    };

    std::uint16_t categoryIdFor(std::string_view name);

    std::unordered_map<GizmoId, Entry> entries_;
    std::unordered_map<std::string, std::uint16_t> categoryToId_;
    std::vector<bool> categoryEnabled_;
    std::uint32_t nextId_ = 1;
    bool masterEnabled_ = true;
};

}  // namespace iron
