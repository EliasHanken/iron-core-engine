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
#include <cmath>

#define IMVIEWGUIZMO_IMPLEMENTATION
#include <ImViewGuizmo.h>

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
    ImViewGuizmo::BeginFrame();

    // Scale the library widget so its rendered diameter matches `size`. The
    // library's default is 256.f * style.scale; we override scale every frame
    // so callers get a predictable on-screen size.
    ImViewGuizmo::GetStyle().scale = size / 256.0f;

    const ImVec2 viewportSize = ImGui::GetMainViewport()->Size;
    const ImVec2 viewportPos  = ImGui::GetMainViewport()->Pos;

    // The overlay window is sized to JUST the gizmo + Iso button area — not
    // fullscreen — so ImGui's WantCaptureMouse stays false outside the corner
    // and WASD / RMB-look continue to work in the rest of the viewport.
    constexpr float kButtonHeight = 26.0f;  // approximate ImGui button height
    constexpr float kPad          = 6.0f;   // visual padding around widget
    const float windowW = size + kPad * 2.0f;
    const float windowH = size + kPad * 2.0f + kButtonHeight + 4.0f;

    ImGui::SetNextWindowPos({viewportPos.x + viewportSize.x - windowW - margin,
                             viewportPos.y + margin});
    ImGui::SetNextWindowSize({windowW, windowH});
    ImGui::SetNextWindowBgAlpha(0.0f);
    constexpr ImGuiWindowFlags kOverlayFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##viewgizmo_overlay", nullptr, kOverlayFlags);

    // Gizmo CENTER in screen space (library treats `position` as the center).
    const ImVec2 windowOrigin = ImGui::GetWindowPos();
    const ImVec2 gizmoCenter{windowOrigin.x + kPad + size * 0.5f,
                             windowOrigin.y + kPad + size * 0.5f};

    // Adapter: iron radians → degrees → quat → glm.
    glm::vec3 pos = toGlm(cam.position);
    glm::quat rot = toGlm(eulerToQuat({cam.pitch * kRad2Deg,
                                       cam.yaw   * kRad2Deg,
                                       0.0f}));
    const glm::vec3 pivot{0.0f, 0.0f, 0.0f};  // orbit around world origin

    bool changed = ImViewGuizmo::Rotate(pos, rot, pivot, gizmoCenter);
    if (changed) {
        cam.position = fromGlm(pos);
        const Vec3 eDeg = quatToEuler(fromGlm(rot));
        cam.pitch = eDeg.x * kDeg2Rad;
        cam.yaw   = eDeg.y * kDeg2Rad;
        // eDeg.z (roll) is discarded — yaw/pitch can't carry it.
    }

    // Iso button below the gizmo, using window-relative coordinates so it
    // sits inside the (small) overlay window's hit zone.
    ImGui::SetCursorPos({kPad + size * 0.25f, kPad + size + 4.0f});
    if (ImGui::Button("Iso", ImVec2(size * 0.5f, 0.0f))) {
        setIsometricView(cam);
        changed = true;
    }

    ImGui::End();
    return changed;
}

}  // namespace iron
