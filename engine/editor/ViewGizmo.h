#pragma once

// Public API for the editor's view-nav widget (ImViewGuizmo drop-in).
// Header is consumer-safe: no ImGui types, no GLM types, no library types.
// The library include + IMVIEWGUIZMO_IMPLEMENTATION lives only in ViewGizmo.cpp.

namespace iron {

struct FreeFlyCamera;

// Draws the ImViewGuizmo axis-handle cube in the top-right corner of the
// current ImGui main viewport plus a small "Iso" reset button immediately
// below it. Applies any user-driven camera changes back to `cam`. Returns
// true if the user interacted this frame.
//
// Must be called inside an ImGui frame (between ImGuiLayer::beginFrame and
// ImGuiLayer::render). Place AFTER your other panels and BEFORE
// imguiLayer.render().
bool drawViewGizmo(FreeFlyCamera& cam, float size = 100.0f, float margin = 20.0f);

// Snap a FreeFlyCamera to the stock 3/4 isometric pose: camera at
// (distance, distance, distance) looking toward world origin.
void setIsometricView(FreeFlyCamera& cam, float distance = 10.0f);

}  // namespace iron
