#pragma once

#include "math/Quaternion.h"
#include "math/Ray.h"
#include "math/Vec.h"

namespace iron {

struct SceneEntity;
class Renderer;

enum class GizmoMode { Translate, Rotate, Scale };
enum class GizmoSpace { World, Local };

// A transform gizmo for the editor, in World or Local space. Operates on a
// single SceneEntity: hit-tests its handles against a mouse ray (for hover +
// click-grab), drags translate/rotate/scale, and draws itself as always-on-top
// debug lines at an explicit origin (the entity's pivot, supplied by the host).
class Gizmo {
public:
    void setMode(GizmoMode m);          // ignored mid-drag
    GizmoMode mode() const { return mode_; }
    bool dragging() const { return axis_ >= 0; }

    void       setSpace(GizmoSpace s);  // ignored mid-drag, like setMode
    GizmoSpace space() const { return space_; }
    void       toggleSpace();           // World<->Local, ignored mid-drag

    // Per-frame input. `origin` is where the gizmo is drawn + hit-tested (the
    // selected entity's world-AABB center). `mousePressed` = LMB went down this
    // frame; `mouseDown` = LMB held. Updates the hovered handle every frame; on a
    // press over a handle it begins a drag and returns true (host should NOT
    // re-select); while dragging it applies the transform to `e` and returns
    // true. Returns false when not consuming the mouse.
    bool update(SceneEntity& e, Vec3 origin, const Ray& mouseRay,
                bool mousePressed, bool mouseDown, Vec3 camPos, float fovDeg);

    // Emit the gizmo at `origin` as always-on-top debug lines; the hovered/active
    // handle is brightened.
    // `rotation` orients the handles in Local space (and is ignored in World
    // space / for Scale, which is always local).
    void draw(Renderer& renderer, Vec3 origin, Quat rotation, Vec3 camPos,
              float fovDeg) const;

private:
    GizmoMode  mode_  = GizmoMode::Translate;
    GizmoSpace space_ = GizmoSpace::World;
    int   axis_ = -1;            // dragging axis 0/1/2; -1 = idle
    int   hoveredAxis_ = -1;     // handle under the cursor (for highlight), -1 = none
    float startParam_ = 0.0f;    // axis param (translate/scale), or tangent displacement (rotate), at drag start
    float lastParam_ = 0.0f;     // last valid param this drag (held on degenerate solves)
    Vec3  startPos_{};
    Vec3  startScale_{};
    Vec3  startOrigin_{};        // gizmo origin captured at drag start (stable axis line)
    Vec3  startHit_{};           // plane hit point at drag start (planar / center handles)
    Vec3  startNormal_{};        // camera-facing plane normal at drag start (center handle)
    Vec3  startTangent_{};       // frozen screen-space ring tangent at drag start (rotate tangent-drag)
    Quat  startRot_{};
};

}  // namespace iron
