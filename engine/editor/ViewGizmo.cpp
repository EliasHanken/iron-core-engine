#include "editor/ViewGizmo.h"

#include "math/Quaternion.h"
#include "math/Vec.h"
#include "scene/FreeFlyCamera.h"

#include <imgui.h>

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

// ImViewGuizmo's implementation uses std::array / std::sort / std::min / std::max
// internally but doesn't include the corresponding standard headers itself —
// surface them here so the single TU that defines IMVIEWGUIZMO_IMPLEMENTATION
// compiles cleanly.
#include <algorithm>
#include <array>

#define IMVIEWGUIZMO_IMPLEMENTATION
#include <ImViewGuizmo.h>

#include <cmath>

namespace iron {

namespace {

constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;
constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;

// Type conversion helpers — same component layout for vectors, so it's a copy.
inline glm::vec3 toGlm(const Vec3& v) { return glm::vec3{v.x, v.y, v.z}; }
inline Vec3      fromGlm(const glm::vec3& v) { return Vec3{v.x, v.y, v.z}; }

// glm::quat constructor is (w, x, y, z) — note the W comes first!
inline glm::quat toGlm(const Quat& q) { return glm::quat{q.w, q.x, q.y, q.z}; }
inline Quat      fromGlm(const glm::quat& q) { return Quat{q.x, q.y, q.z, q.w}; }

}  // namespace

void setIsometricView(FreeFlyCamera& cam, float distance) {
    // Stock 3/4 isometric pose: camera at (+d, +d, +d) looking at origin.
    // Forward direction = normalize(-1, -1, -1).
    //
    // FreeFlyCamera::forward() = { -sin(yaw)*cos(pitch), sin(pitch), -cos(yaw)*cos(pitch) }
    // Solving for forward = (-1/√3, -1/√3, -1/√3):
    //   sin(pitch) = -1/√3 → pitch = -asin(1/√3) ≈ -35.26°
    //   sin(yaw)*cos(pitch) = +1/√3, cos(pitch) = √(2/3) → sin(yaw) = +1/√2
    //   → yaw = +π/4
    cam.position = {distance, distance, distance};
    const Vec3 forward = normalize(Vec3{-1.0f, -1.0f, -1.0f});
    cam.yaw   = std::atan2(-forward.x, -forward.z);  // +π/4
    cam.pitch = std::asin(forward.y);                // -asin(1/√3)
}

bool drawViewGizmo(FreeFlyCamera& cam, float size, float margin) {
    const ImVec2 viewportSize = ImGui::GetMainViewport()->Size;
    const ImVec2 gizmoPos{viewportSize.x - size - margin, margin};

    // Convert iron::FreeFlyCamera state → glm types for the library call.
    // eulerToQuat / quatToEuler operate in DEGREES (per Quaternion.h docs);
    // cam.yaw / cam.pitch are RADIANS. Convert at the boundary.
    glm::vec3 pos = toGlm(cam.position);
    glm::quat rot = toGlm(eulerToQuat({cam.pitch * kRad2Deg,
                                       cam.yaw   * kRad2Deg,
                                       0.0f}));

    // Pivot at the world origin — standard orbit-around-scene behaviour.
    const glm::vec3 pivot{0.0f, 0.0f, 0.0f};
    bool changed = ImViewGuizmo::Rotate(pos, rot, pivot, gizmoPos);
    if (changed) {
        cam.position = fromGlm(pos);
        const Vec3 eDeg = quatToEuler(fromGlm(rot));
        cam.pitch = eDeg.x * kDeg2Rad;
        cam.yaw   = eDeg.y * kDeg2Rad;
        // eDeg.z (roll) is discarded — yaw/pitch can't carry it.
        // Axis-snap produces zero roll; pure drag-orbit may lose it (v1 limit).
    }

    // "Iso" button immediately below the gizmo, centered horizontally.
    ImGui::SetCursorScreenPos({gizmoPos.x + size * 0.25f, gizmoPos.y + size + 4.0f});
    if (ImGui::Button("Iso", ImVec2(size * 0.5f, 0.0f))) {
        setIsometricView(cam);
        changed = true;
    }
    return changed;
}

}  // namespace iron
