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
bool drawViewGizmo(FreeFlyCamera& cam,
                   Vec3 pivot = Vec3{0.0f, 0.0f, 0.0f},
                   float size = 150.0f,
                   float margin = 20.0f);

// Snap a FreeFlyCamera to a stock 3/4 isometric pose centered on `pivot`.
// Camera goes to pivot + (distance, distance, distance) looking back at
// pivot. With pivot = world origin (the default), this is the same pose
// the test_iso_view tests cover.
void setIsometricView(FreeFlyCamera& cam,
                      Vec3 pivot = Vec3{0.0f, 0.0f, 0.0f},
                      float distance = 10.0f);

}  // namespace iron
