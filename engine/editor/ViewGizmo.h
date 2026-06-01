#pragma once

#include "math/Vec.h"

// Public API for the editor's view-nav widget (ImViewGuizmo drop-in).
// Header is consumer-safe: no ImGui types, no GLM types, no library types.

namespace iron {

struct FreeFlyCamera;

// Draws the ImViewGuizmo axis-handle cube in the top-right corner of the
// current ImGui main viewport plus an "Iso" reset button below it. The
// camera orbits drag-rotations around `pivot` (typically the selected
// entity's world position; pass world origin {0,0,0} for "no selection").
// Returns true if the user interacted this frame.
//
// Must be called inside an ImGui frame (between ImGuiLayer::beginFrame and
// ImGuiLayer::render). Place AFTER your other panels and BEFORE
// imguiLayer.render().
//
// `rectMin` + `rectSize` (window-space pixels) define the rectangle the gizmo
// positions itself within (top-right corner). Pass {0,0}+{0,0} (default) to use
// the ImGui main viewport work-area + foreground draw list (legacy behavior);
// pass a panel's rect to anchor + clip the gizmo inside that panel.
bool drawViewGizmo(FreeFlyCamera& cam,
                   Vec3 pivot = Vec3{0.0f, 0.0f, 0.0f},
                   float size = 150.0f,
                   float margin = 20.0f,
                   Vec2 rectMin = Vec2{0.0f, 0.0f},
                   Vec2 rectSize = Vec2{0.0f, 0.0f});

// Snap a FreeFlyCamera to a stock 3/4 isometric pose centered on `pivot`.
// Camera goes to pivot + (distance, distance, distance) looking back at
// pivot. With pivot = world origin (the default), this is the same pose
// the test_iso_view tests cover.
void setIsometricView(FreeFlyCamera& cam,
                      Vec3 pivot = Vec3{0.0f, 0.0f, 0.0f},
                      float distance = 10.0f);

// Orbit the camera around `pivot` by (dYaw, dPitch) radians. Preserves the
// current distance from pivot. Used by the gizmo's drag-orbit internally
// AND by middle-mouse-button drag in the sandbox.
void orbitCamera(FreeFlyCamera& cam, Vec3 pivot, float dYaw, float dPitch);

}  // namespace iron
