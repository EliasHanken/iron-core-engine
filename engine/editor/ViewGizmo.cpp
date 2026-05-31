#include "editor/ViewGizmo.h"

#include "math/Quaternion.h"
#include "math/Vec.h"
#include "scene/FreeFlyCamera.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace iron {

namespace {

constexpr float kPi      = 3.14159265358979323846f;
constexpr float kPitchClamp = 89.0f * (kPi / 180.0f);   // ±89° to avoid gimbal flip
constexpr float kOrbitSensitivity = 0.0075f;             // radians per pixel

// One axis handle: world-space axis direction + RGB color + label.
struct AxisHandle {
    Vec3        worldDir;
    ImU32       color;
    const char* label;
};

const std::array<AxisHandle, 6> kAxes = {
    AxisHandle{ Vec3{ 1.0f,  0.0f,  0.0f}, IM_COL32(220,  60,  60, 255), "X"  },
    AxisHandle{ Vec3{-1.0f,  0.0f,  0.0f}, IM_COL32(160,  60,  60, 255), "-X" },
    AxisHandle{ Vec3{ 0.0f,  1.0f,  0.0f}, IM_COL32( 70, 200,  70, 255), "Y"  },
    AxisHandle{ Vec3{ 0.0f, -1.0f,  0.0f}, IM_COL32( 60, 140,  60, 255), "-Y" },
    AxisHandle{ Vec3{ 0.0f,  0.0f,  1.0f}, IM_COL32( 80, 130, 230, 255), "Z"  },
    AxisHandle{ Vec3{ 0.0f,  0.0f, -1.0f}, IM_COL32( 60,  90, 170, 255), "-Z" },
};

// Compute the camera's right / up / forward basis from yaw and pitch.
// Convention matches FreeFlyCamera: yaw=0, pitch=0 looks toward world -Z, +Y up.
struct CameraBasis {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
};

CameraBasis cameraBasisFromYawPitch(float yaw, float pitch) {
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    // Forward matches FreeFlyCamera::forward() exactly.
    const Vec3 fwd{-sy * cp, sp, -cy * cp};
    // Right is forward × world-up, then re-orthogonalized for safety.
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};
    const Vec3 right = normalize(cross(fwd, worldUp));
    const Vec3 up    = cross(right, fwd);   // already unit
    return {right, up, fwd};
}

// Project a world-space direction into the gizmo's screen-space using the
// camera basis. Returns:
//   .screenX, .screenY : 2D offset from the gizmo center, in pixels
//   .depth             : z-depth in camera space (negative = in front of camera)
struct ProjectedHandle {
    float screenX;
    float screenY;
    float depth;
};

ProjectedHandle projectAxis(Vec3 worldDir, const CameraBasis& basis, float radiusPx) {
    // Component along each basis axis = dot(worldDir, basis_axis).
    const float r = dot(worldDir, basis.right);
    const float u = dot(worldDir, basis.up);
    const float f = dot(worldDir, basis.forward);
    // Screen X is +right, screen Y is +down (ImGui convention) — so invert u.
    return {r * radiusPx, -u * radiusPx, f};
}

}  // namespace

void setIsometricView(FreeFlyCamera& cam, Vec3 pivot, float distance) {
    // Stock 3/4 isometric pose around `pivot`. Camera at pivot+(d,d,d)
    // looking back toward pivot. Forward direction normalize(-1,-1,-1).
    cam.position = {pivot.x + distance,
                    pivot.y + distance,
                    pivot.z + distance};
    const Vec3 forward = normalize(Vec3{-1.0f, -1.0f, -1.0f});
    cam.yaw   = std::atan2(-forward.x, -forward.z);
    cam.pitch = std::asin(forward.y);
}

namespace {

// Snap the camera to look at `pivot` from along the world axis `axisDir`,
// preserving current distance-to-pivot. The view direction becomes -axisDir;
// derive yaw/pitch from that.
void snapToAxis(FreeFlyCamera& cam, Vec3 pivot, Vec3 axisDir) {
    const float distance = std::max(0.001f, length(cam.position - pivot));
    cam.position = {pivot.x + axisDir.x * distance,
                    pivot.y + axisDir.y * distance,
                    pivot.z + axisDir.z * distance};
    // View direction is from camera toward pivot, i.e. -axisDir.
    // From FreeFlyCamera::forward() = {-sy*cp, sp, -cy*cp}:
    //   sp = -axisDir.y → pitch = asin(-axisDir.y), but clamped.
    //   yaw = atan2(axisDir.x, axisDir.z) (derived by matching components)
    Vec3 view = Vec3{-axisDir.x, -axisDir.y, -axisDir.z};
    float pitch = std::asin(view.y);
    pitch = std::max(-kPitchClamp, std::min(kPitchClamp, pitch));
    cam.pitch = pitch;
    cam.yaw   = std::atan2(-view.x, -view.z);
}

// Apply a one-frame drag: rotate the camera around `pivot` by (dYaw, dPitch).
// Preserves the orbit radius.
void orbitAroundPivot(FreeFlyCamera& cam, Vec3 pivot, float dYaw, float dPitch) {
    const float distance = std::max(0.001f, length(cam.position - pivot));
    cam.yaw   += dYaw;
    cam.pitch += dPitch;
    cam.pitch = std::max(-kPitchClamp, std::min(kPitchClamp, cam.pitch));
    // Re-derive position so the camera sits on the orbit sphere around pivot,
    // facing the pivot (position = pivot - forward * distance).
    const CameraBasis b = cameraBasisFromYawPitch(cam.yaw, cam.pitch);
    cam.position = {pivot.x - b.forward.x * distance,
                    pivot.y - b.forward.y * distance,
                    pivot.z - b.forward.z * distance};
}

}  // namespace

bool drawViewGizmo(FreeFlyCamera& cam, Vec3 pivot, float size, float margin) {
    // Persistent drag state — we own this, no library involved.
    static bool  dragActive   = false;
    static int   dragButton   = -1;
    static ImVec2 lastMouse{0.0f, 0.0f};

    const ImVec2 viewportSize = ImGui::GetMainViewport()->Size;
    const ImVec2 viewportPos  = ImGui::GetMainViewport()->Pos;

    // Small overlay window — sized to JUST the gizmo + Iso button so WASD
    // stays free outside the corner.
    constexpr float kButtonHeight = 26.0f;
    constexpr float kPad          = 6.0f;
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

    const ImVec2 windowOrigin = ImGui::GetWindowPos();
    const ImVec2 gizmoCenter{windowOrigin.x + kPad + size * 0.5f,
                             windowOrigin.y + kPad + size * 0.5f};
    const float  gizmoRadius   = size * 0.36f;     // axis tip distance from center
    const float  handleRadius  = size * 0.10f;     // hit + draw radius per handle

    // Project all 6 handles to screen space using the current camera basis.
    const CameraBasis basis = cameraBasisFromYawPitch(cam.yaw, cam.pitch);
    struct Drawable { ImVec2 pos; float depth; size_t axisIdx; float radius; };
    std::array<Drawable, 6> drawables{};
    for (size_t i = 0; i < kAxes.size(); ++i) {
        const ProjectedHandle p = projectAxis(kAxes[i].worldDir, basis, gizmoRadius);
        // Shrink handles behind the gizmo center for depth feedback.
        const float depthScale = (p.depth >= 0.0f) ? 0.65f : 1.0f;
        drawables[i] = Drawable{
            ImVec2{gizmoCenter.x + p.screenX, gizmoCenter.y + p.screenY},
            p.depth,
            i,
            handleRadius * depthScale,
        };
    }
    // Sort so larger depth (farther from camera) is drawn first — front handles
    // overdraw back handles.
    std::sort(drawables.begin(), drawables.end(),
              [](const Drawable& a, const Drawable& b) { return a.depth > b.depth; });

    // Invisible button covers the whole gizmo area for input handling.
    const ImVec2 buttonOrigin{windowOrigin.x + kPad,
                              windowOrigin.y + kPad};
    ImGui::SetCursorScreenPos(buttonOrigin);
    ImGui::InvisibleButton("##viewgizmo_hit", ImVec2(size, size));
    const bool  hovered    = ImGui::IsItemHovered();
    const ImVec2 mousePos  = ImGui::GetIO().MousePos;

    // Find which handle the mouse is closest to (within radius).
    int hoveredAxis = -1;
    if (hovered) {
        float bestDist = handleRadius * handleRadius;
        for (const auto& d : drawables) {
            const float dx = mousePos.x - d.pos.x;
            const float dy = mousePos.y - d.pos.y;
            const float distSq = dx * dx + dy * dy;
            if (distSq < bestDist) {
                bestDist = distSq;
                hoveredAxis = static_cast<int>(d.axisIdx);
            }
        }
    }

    bool changed = false;

    // Click / drag handling.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoveredAxis >= 0) {
            // Click on a handle → snap to that axis view.
            snapToAxis(cam, pivot, kAxes[hoveredAxis].worldDir);
            changed = true;
        } else {
            // Click on empty gizmo area → begin drag-orbit.
            dragActive = true;
            dragButton = ImGuiMouseButton_Left;
            lastMouse  = mousePos;
        }
    }
    if (dragActive) {
        if (!ImGui::IsMouseDown(static_cast<ImGuiMouseButton>(dragButton))) {
            dragActive = false;
            dragButton = -1;
        } else {
            const float dx = mousePos.x - lastMouse.x;
            const float dy = mousePos.y - lastMouse.y;
            if (dx != 0.0f || dy != 0.0f) {
                orbitAroundPivot(cam, pivot,
                                 -dx * kOrbitSensitivity,
                                 -dy * kOrbitSensitivity);
                lastMouse = mousePos;
                changed = true;
            }
        }
    }

    // Render: connecting lines from center to each handle, then circles, then labels.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Center dot.
    dl->AddCircleFilled(gizmoCenter, 3.0f, IM_COL32(220, 220, 220, 200));
    // Pre-pass: lines for the FRONT 3 handles (depth < 0). Back lines are noisy.
    for (const auto& d : drawables) {
        if (d.depth < 0.0f) {
            const ImU32 col = kAxes[d.axisIdx].color;
            dl->AddLine(gizmoCenter, d.pos, col, 2.0f);
        }
    }
    // Handles + labels.
    for (const auto& d : drawables) {
        ImU32 col = kAxes[d.axisIdx].color;
        const bool isHovered = (static_cast<int>(d.axisIdx) == hoveredAxis);
        if (isHovered) {
            // Brighten on hover.
            ImVec4 colVec = ImGui::ColorConvertU32ToFloat4(col);
            colVec.x = std::min(1.0f, colVec.x + 0.25f);
            colVec.y = std::min(1.0f, colVec.y + 0.25f);
            colVec.z = std::min(1.0f, colVec.z + 0.25f);
            col = ImGui::ColorConvertFloat4ToU32(colVec);
        }
        const float r = isHovered ? d.radius * 1.2f : d.radius;
        dl->AddCircleFilled(d.pos, r, col, 16);
        dl->AddCircle(d.pos, r, IM_COL32(20, 20, 20, 200), 16, 1.5f);
        // Label centered on the handle. Use ImGui's default font.
        const char* label = kAxes[d.axisIdx].label;
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        const ImVec2 textPos{d.pos.x - textSize.x * 0.5f, d.pos.y - textSize.y * 0.5f};
        dl->AddText(textPos, IM_COL32(20, 20, 20, 255), label);
    }

    // Iso button — uses the same pivot the gizmo orbits around.
    ImGui::SetCursorPos({kPad + size * 0.25f, kPad + size + 4.0f});
    if (ImGui::Button("Iso", ImVec2(size * 0.5f, 0.0f))) {
        setIsometricView(cam, pivot);
        changed = true;
    }

    ImGui::End();
    return changed;
}

}  // namespace iron
