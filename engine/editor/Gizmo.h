#pragma once

#include "math/Quaternion.h"
#include "math/Ray.h"
#include "math/Vec.h"

namespace iron {

struct SceneEntity;
class Renderer;

enum class GizmoMode { Translate, Rotate, Scale };

// A world-axis transform gizmo for the editor. Operates on a single SceneEntity:
// hit-tests its handles against a mouse ray, drags translate/rotate/scale, and
// draws itself as debug lines. World-axis only (v1).
class Gizmo {
public:
    void setMode(GizmoMode m);          // ignored mid-drag
    GizmoMode mode() const { return mode_; }
    bool dragging() const { return axis_ >= 0; }

    // Per-frame input. `mousePressed` = left button went down this frame;
    // `mouseDown` = left button held. On press, hit-tests handles and may start a
    // drag; while dragging, applies the transform to `e`. Returns true when the
    // gizmo is consuming the mouse this frame (the host should not re-select).
    bool update(SceneEntity& e, const Ray& mouseRay, bool mousePressed,
                bool mouseDown, Vec3 camPos);

    // Emit the gizmo at `e` as debug lines (X red, Y green, Z blue).
    void draw(Renderer& renderer, const SceneEntity& e, Vec3 camPos) const;

private:
    GizmoMode mode_ = GizmoMode::Translate;
    int   axis_ = -1;            // dragging axis 0/1/2; -1 = idle
    float startParam_ = 0.0f;    // axis param (translate/scale) or angle (rotate) at drag start
    Vec3  startPos_{};
    Vec3  startScale_{};
    Quat  startRot_{};
};

}  // namespace iron
