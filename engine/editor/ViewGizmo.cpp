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

void orbitCamera(FreeFlyCamera& cam, Vec3 pivot, float dYaw, float dPitch) {
    const float distance = std::max(0.001f, length(cam.position - pivot));
    cam.yaw   += dYaw;
    cam.pitch += dPitch;
    cam.pitch = std::max(-kPitchClamp, std::min(kPitchClamp, cam.pitch));
    // Re-derive position so the camera stays on the orbit sphere facing pivot
    // (position = pivot - forward * distance). `forward()` is the public
    // FreeFlyCamera accessor — no internal basis helper needed.
    const Vec3 fwd = cam.forward();
    cam.position = {pivot.x - fwd.x * distance,
                    pivot.y - fwd.y * distance,
                    pivot.z - fwd.z * distance};
}

namespace {

// ── Camera tween ─────────────────────────────────────────────────────────
// Smooth in-flight transition between two camera poses, parameterised by
// distance-to-pivot + a unit direction so the camera arcs around the pivot
// instead of dipping through it.
struct CameraTween {
    bool  active     = false;
    float elapsed    = 0.0f;
    float duration   = 0.0f;
    Vec3  pivot{};
    Vec3  startDir{};      // unit, points from pivot toward camera at start
    Vec3  targetDir{};     // unit
    float startDist  = 0.0f;
    float targetDist = 0.0f;
    float startYaw   = 0.0f;
    float targetYaw  = 0.0f;
    float startPitch = 0.0f;
    float targetPitch = 0.0f;
};

constexpr float kTweenDuration = 0.30f;   // seconds, eyeballed

// Ease-in-out: t * t * (3 - 2t).
float smoothstep01(float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    return t * t * (3.0f - 2.0f * t);
}

// Shortest-path angular interpolation. Input angles in radians (any range);
// output is angle-wrapped lerp from `a` to `b`.
float lerpAngle(float a, float b, float t) {
    float delta = b - a;
    while (delta >  kPi) delta -= 2.0f * kPi;
    while (delta < -kPi) delta += 2.0f * kPi;
    return a + delta * t;
}

// Spherical lerp of two unit vectors. Used to arc the camera around the
// pivot instead of dipping through it.
Vec3 slerpDir(Vec3 a, Vec3 b, float t) {
    float d = dot(a, b);
    d = std::max(-1.0f, std::min(1.0f, d));
    const float angle = std::acos(d);
    if (angle < 1e-4f) return a;                       // already aligned
    const float sinAngle = std::sin(angle);
    const float wA = std::sin((1.0f - t) * angle) / sinAngle;
    const float wB = std::sin(t * angle) / sinAngle;
    return Vec3{a.x * wA + b.x * wB,
                a.y * wA + b.y * wB,
                a.z * wA + b.z * wB};
}

// Begin a tween from cam's current pose to (targetPos, targetYaw, targetPitch).
void beginTween(CameraTween& tw, const FreeFlyCamera& cam, Vec3 pivot,
                Vec3 targetPos, float targetYaw, float targetPitch) {
    const Vec3 startRel  = cam.position - pivot;
    const Vec3 targetRel = targetPos    - pivot;
    tw.active       = true;
    tw.elapsed      = 0.0f;
    tw.duration     = kTweenDuration;
    tw.pivot        = pivot;
    tw.startDist    = std::max(0.001f, length(startRel));
    tw.targetDist   = std::max(0.001f, length(targetRel));
    tw.startDir     = startRel  * (1.0f / tw.startDist);
    tw.targetDir    = targetRel * (1.0f / tw.targetDist);
    tw.startYaw     = cam.yaw;
    tw.targetYaw    = targetYaw;
    tw.startPitch   = cam.pitch;
    tw.targetPitch  = targetPitch;
}

// Advance an active tween by dt seconds. Writes interpolated state to `cam`.
// Returns true while the tween is still in flight, false once it completes.
bool tickTween(CameraTween& tw, FreeFlyCamera& cam, float dt) {
    if (!tw.active) return false;
    tw.elapsed += dt;
    const float raw  = std::min(1.0f, tw.elapsed / tw.duration);
    const float t    = smoothstep01(raw);
    const Vec3  dir  = normalize(slerpDir(tw.startDir, tw.targetDir, t));
    const float dist = tw.startDist + (tw.targetDist - tw.startDist) * t;
    cam.position = {tw.pivot.x + dir.x * dist,
                    tw.pivot.y + dir.y * dist,
                    tw.pivot.z + dir.z * dist};
    cam.yaw   = lerpAngle(tw.startYaw, tw.targetYaw, t);
    cam.pitch = tw.startPitch + (tw.targetPitch - tw.startPitch) * t;
    if (raw >= 1.0f) tw.active = false;
    return tw.active;
}

// ── FOV tween (independent of CameraTween) ───────────────────────────────
// Snapped views (axis handles + Iso) feel "isometric" by dropping the FOV
// to a telephoto value, which flattens the perspective enough to read as
// orthographic without changing the projection matrix. Drag-orbit returns
// to a natural perspective FOV. The tween runs in its own state so it can
// run alongside drag-orbit without fighting CameraTween's per-frame
// pos/yaw/pitch writes.
constexpr float kDefaultFov = 60.0f;   // matches FreeFlyCamera::fovDeg default
constexpr float kSnapFov    = 30.0f;   // telephoto — flat-perspective "iso feel"
constexpr float kFovTweenDuration = 0.30f;

struct FovTween {
    bool  active   = false;
    float elapsed  = 0.0f;
    float duration = kFovTweenDuration;
    float startFov = 0.0f;
    float targetFov = 0.0f;
};

void beginFovTween(FovTween& tw, const FreeFlyCamera& cam, float targetFov) {
    tw.active    = true;
    tw.elapsed   = 0.0f;
    tw.duration  = kFovTweenDuration;
    tw.startFov  = cam.fovDeg;
    tw.targetFov = targetFov;
}

bool tickFovTween(FovTween& tw, FreeFlyCamera& cam, float dt) {
    if (!tw.active) return false;
    tw.elapsed += dt;
    const float raw = std::min(1.0f, tw.elapsed / tw.duration);
    const float t   = smoothstep01(raw);
    cam.fovDeg = tw.startFov + (tw.targetFov - tw.startFov) * t;
    if (raw >= 1.0f) tw.active = false;
    return tw.active;
}

// Compute the target camera state for a click-snap-to-axis WITHOUT applying it.
// Caller passes the result to beginTween or applies directly.
struct CameraPose {
    Vec3  position;
    float yaw;
    float pitch;
};

CameraPose computeAxisSnapPose(const FreeFlyCamera& cam, Vec3 pivot, Vec3 axisDir) {
    const float distance = std::max(0.001f, length(cam.position - pivot));
    const Vec3 view = Vec3{-axisDir.x, -axisDir.y, -axisDir.z};
    float pitch = std::asin(view.y);
    pitch = std::max(-kPitchClamp, std::min(kPitchClamp, pitch));
    return CameraPose{
        Vec3{pivot.x + axisDir.x * distance,
             pivot.y + axisDir.y * distance,
             pivot.z + axisDir.z * distance},
        std::atan2(-view.x, -view.z),
        pitch,
    };
}

// Compute the target camera state for an iso snap that PRESERVES the current
// camera distance from pivot (so Iso just rotates around, no zoom-far-out).
CameraPose computeIsoPose(const FreeFlyCamera& cam, Vec3 pivot) {
    const Vec3 isoDir = normalize(Vec3{1.0f, 1.0f, 1.0f});
    const float distance = std::max(0.001f, length(cam.position - pivot));
    const Vec3 forward = normalize(Vec3{-1.0f, -1.0f, -1.0f});
    return CameraPose{
        Vec3{pivot.x + isoDir.x * distance,
             pivot.y + isoDir.y * distance,
             pivot.z + isoDir.z * distance},
        std::atan2(-forward.x, -forward.z),
        std::asin(forward.y),
    };
}

}  // namespace

bool drawViewGizmo(FreeFlyCamera& cam, Vec3 pivot, float size, float margin) {
    // Persistent state — we own this, no library involved.
    static bool       dragActive = false;
    static int        dragButton = -1;
    static ImVec2     lastMouse{0.0f, 0.0f};
    static CameraTween tween;
    static FovTween    fovTween;
    static bool       isoMode    = false;

    // Advance any in-flight camera tween BEFORE input handling, so a fresh
    // user drag can cancel it cleanly.
    bool changed = false;
    const float dt = ImGui::GetIO().DeltaTime;
    if (tween.active) {
        tickTween(tween, cam, dt);
        changed = true;
    }
    if (fovTween.active) {
        tickFovTween(fovTween, cam, dt);
        changed = true;
    }

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

    // Click / drag handling.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoveredAxis >= 0) {
            // Click on a handle → tween orientation to that axis view. FOV is
            // governed solely by isoMode (the Iso button toggles it); axis
            // clicks no longer touch FOV.
            const CameraPose target = computeAxisSnapPose(cam, pivot,
                                                          kAxes[hoveredAxis].worldDir);
            beginTween(tween, cam, pivot,
                       target.position, target.yaw, target.pitch);
            changed = true;
        } else {
            // Click on empty gizmo area → begin drag-orbit. Cancel any
            // in-flight pos/orientation tween. FOV is untouched so the user
            // can rotate while staying in iso mode.
            tween.active = false;
            dragActive   = true;
            dragButton   = ImGuiMouseButton_Left;
            lastMouse    = mousePos;
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
                orbitCamera(cam, pivot,
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

    // Iso button — uses the same pivot the gizmo orbits around. Capture the
    // pre-click iso state so Push/Pop pair stays balanced even if the click
    // flips isoMode mid-block.
    ImGui::SetCursorPos({kPad + size * 0.25f, kPad + size + 4.0f});
    const bool wasIsoActive = isoMode;
    if (wasIsoActive) {
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("Iso", ImVec2(size * 0.5f, 0.0f))) {
        // Toggle iso mode. Entering: orbit to iso pose + drop FOV (telephoto
        // = orthographic feel). Exiting: only restore FOV; keep current
        // orientation/position so the user doesn't get teleported back.
        isoMode = !isoMode;
        if (isoMode) {
            const CameraPose target = computeIsoPose(cam, pivot);
            beginTween(tween, cam, pivot,
                       target.position, target.yaw, target.pitch);
            beginFovTween(fovTween, cam, kSnapFov);
        } else {
            beginFovTween(fovTween, cam, kDefaultFov);
        }
        changed = true;
    }
    if (wasIsoActive) ImGui::PopStyleColor();

    ImGui::End();
    return changed;
}

}  // namespace iron
