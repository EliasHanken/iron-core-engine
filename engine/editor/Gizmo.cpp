#include "editor/Gizmo.h"

#include "render/Renderer.h"
#include "scene/SceneFormat.h"

#include <cmath>

namespace iron {

namespace {

constexpr float kGizmoScreenScale = 0.15f;  // gizmo world size ~= this * camera distance
constexpr float kHandlePickFrac   = 0.25f;  // forgiving axis pick threshold (fraction of size)
constexpr float kPlaneInset       = 0.08f;  // planar-handle square inner corner (fraction of size)
constexpr float kPlaneReach       = 0.30f;  // planar-handle square outer corner (fraction of size)
constexpr float kCenterPickFrac   = 0.12f;  // center free-move handle pick radius (fraction of size)
constexpr float kCenterHalf       = 0.09f;  // center handle half-size (fraction of size)
constexpr float kRingPickFrac     = 0.20f;  // rotate-ring pick band, screen-space (fraction of size)
constexpr float kTwoPi            = 6.28318530718f;

// Handle ids: 0/1/2 = single axes, 3/4/5 = planar (3 + plane-normal-axis,
// Translate only), 6 = center free-move (Translate only).
constexpr int kCenterHandle = 6;

Vec3 axisDir(int a)   { return a == 0 ? Vec3{1, 0, 0} : a == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1}; }
Vec3 axisColor(int a) { return a == 0 ? Vec3{1.0f, 0.25f, 0.25f}
                             : a == 1 ? Vec3{0.25f, 1.0f, 0.25f}
                                      : Vec3{0.35f, 0.45f, 1.0f}; }

// Fill ax[0..2] with the gizmo's handle axes. World: the canonical X/Y/Z. Local:
// the entity's local axes (its rotation applied), expressed in world space.
void buildBasis(bool local, const Quat& rot, Vec3 ax[3]) {
    for (int a = 0; a < 3; ++a)
        ax[a] = local ? rot.rotate(axisDir(a)) : axisDir(a);
}

float gizmoSize(Vec3 camPos, Vec3 origin) {
    const float d = length(camPos - origin) * kGizmoScreenScale;
    return d < 0.001f ? 0.001f : d;
}

// Closest distance between two segments (Ericson, Real-Time Collision Detection 5.1.9).
float segSegDistance(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2) {
    const Vec3 d1 = q1 - p1;
    const Vec3 d2 = q2 - p2;
    const Vec3 r  = p1 - p2;
    const float a = dot(d1, d1);
    const float e = dot(d2, d2);
    const float f = dot(d2, r);
    const float EPS = 1e-8f;
    float s = 0.0f, t = 0.0f;
    if (a <= EPS && e <= EPS) {
        // both points
    } else if (a <= EPS) {
        t = f / e; if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    } else {
        const float c = dot(d1, r);
        if (e <= EPS) {
            s = -c / a; if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
        } else {
            const float b = dot(d1, d2);
            const float denom = a * e - b * b;
            if (denom > EPS) {
                s = (b * f - c * e) / denom;
                if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
            }
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f; s = -c / a;
                if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
            } else if (t > 1.0f) {
                t = 1.0f; s = (b - c) / a;
                if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
            }
        }
    }
    const Vec3 c1 = p1 + d1 * s;
    const Vec3 c2 = p2 + d2 * t;
    return length(c1 - c2);
}

float raySegmentDistance(const Ray& ray, Vec3 a, Vec3 b) {
    return segSegDistance(ray.origin, ray.origin + ray.direction * 1.0e4f, a, b);
}

// Distance from the ray (as an infinite line) to a world point.
float rayPointDistance(const Ray& ray, Vec3 p) {
    const Vec3 w = p - ray.origin;
    const Vec3 closest = ray.origin + ray.direction * dot(w, ray.direction);  // dir unit
    return length(closest - p);
}

// Param along the infinite line (origin + dir*p, dir unit) of the point closest
// to the ray. Returns false (leaves `out` untouched) when the ray is
// near-parallel to the axis (ill-conditioned).
bool rayAxisParam(const Ray& ray, Vec3 origin, Vec3 dir, float& out) {
    const Vec3 r = ray.origin - origin;
    const float b = dot(ray.direction, dir);
    const float c = dot(ray.direction, r);
    const float f = dot(dir, r);
    const float denom = 1.0f - b * b;
    if (denom < 1e-4f) return false;
    out = (f - b * c) / denom;
    return true;
}

bool rayPlane(const Ray& ray, Vec3 origin, Vec3 n, Vec3& hit) {
    const float denom = dot(ray.direction, n);
    if (std::fabs(denom) < 1e-6f) return false;
    const float t = dot(origin - ray.origin, n) / denom;
    if (t < 0.0f) return false;
    hit = ray.origin + ray.direction * t;
    return true;
}

// Camera-facing right/up basis at `origin`. right/up span the screen plane; the
// plane normal (camPos - origin) points toward the camera, so atan2(.,.) in this
// basis increases counter-clockwise as seen by the viewer. Used by the center
// handle billboard and the rotate ring's screen-space picking + angle.
void cameraBasis(Vec3 origin, Vec3 camPos, Vec3& right, Vec3& up) {
    const Vec3 n = normalize(camPos - origin);
    Vec3 r = cross(Vec3{0, 1, 0}, n);
    right = (length(r) < 1e-4f) ? Vec3{1, 0, 0} : normalize(r);
    up = cross(n, right);
}

// The cursor's angle (CCW) around the gizmo center, measured on a camera-facing
// plane through `origin`. Intersecting the cursor ray with THIS plane (always
// fully facing the camera, never edge-on) and taking the angle of (hit - origin)
// pivots the rotation about the gizmo's actual on-screen position — at any screen
// location, any zoom. (Taking atan2 of the ray DIRECTION instead pivots about the
// camera's view axis / screen center, which skews sensitivity and makes straight
// drags saturate when the gizmo is off-center — the M35 bug this fixes.)
// Returns false only when the cursor is exactly on the view axis through origin.
bool screenAngle(const Ray& ray, Vec3 origin, Vec3 camPos, float& out) {
    const Vec3 n = normalize(camPos - origin);   // camera-facing plane normal
    Vec3 hit;
    if (!rayPlane(ray, origin, n, hit)) return false;
    Vec3 right, up;
    cameraBasis(origin, camPos, right, up);
    const Vec3 d = hit - origin;
    const float x = dot(d, right), y = dot(d, up);
    if (x * x + y * y < 1e-12f) return false;
    out = std::atan2(y, x);
    return true;
}

// The two in-plane axes of the plane whose normal is basis-axis `n`.
void planeAxes(const Vec3 ax[3], int n, Vec3& u, Vec3& v) {
    if (n == 0)      { u = ax[1]; v = ax[2]; }  // YZ
    else if (n == 1) { u = ax[0]; v = ax[2]; }  // XZ (horizontal)
    else             { u = ax[0]; v = ax[1]; }  // XY
}

// True if the planar handle for basis-axis `n` is under the ray. The square sits
// in the [inset, reach] corner of the two in-plane axes near origin.
bool planeHandleHit(const Ray& ray, Vec3 origin, const Vec3 ax[3], int n, float size) {
    Vec3 hit;
    if (!rayPlane(ray, origin, ax[n], hit)) return false;
    Vec3 u, v; planeAxes(ax, n, u, v);
    const Vec3 d = hit - origin;
    const float du = dot(d, u), dv = dot(d, v);
    const float lo = kPlaneInset * size, hi = kPlaneReach * size;
    return du >= lo && du <= hi && dv >= lo && dv <= hi;
}

// Which handle is under the ray for the given mode, or -1.
int pickHandle(GizmoMode mode, const Ray& ray, Vec3 origin, const Vec3 ax[3],
               Vec3 camPos, float size) {
    if (mode == GizmoMode::Translate) {
        // Center free-move handle (innermost) wins right at the origin.
        if (rayPointDistance(ray, origin) < size * kCenterPickFrac) return kCenterHandle;
        // Then the planar corner squares.
        for (int n = 0; n < 3; ++n)
            if (planeHandleHit(ray, origin, ax, n, size)) return 3 + n;
    }
    if (mode == GizmoMode::Translate || mode == GizmoMode::Scale) {
        int picked = -1;
        float best = size * kHandlePickFrac;
        for (int a = 0; a < 3; ++a) {
            const float d = raySegmentDistance(ray, origin, origin + ax[a] * size);
            if (d < best) { best = d; picked = a; }
        }
        return picked;
    }
    // Rotate: measure the cursor's distance to each ring in the camera's screen
    // plane, so oblique and edge-on rings stay easy to grab (a ray-vs-plane band
    // misses them). Project the cursor to the gizmo's depth, then sample each ring
    // and keep the nearest within tolerance.
    Vec3 right, up;
    cameraBasis(origin, camPos, right, up);
    const Vec3 n = normalize(camPos - origin);
    const float vd = dot(ray.direction, n);
    if (std::fabs(vd) < 1e-5f) return -1;                 // ray skims the screen plane
    const Vec3 M = camPos + ray.direction * (-length(camPos - origin) / vd);
    const float mx = dot(M - origin, right), my = dot(M - origin, up);

    int picked = -1;
    float best = size * kRingPickFrac;
    constexpr int S = 64;
    for (int a = 0; a < 3; ++a) {
        const Vec3 axisN = ax[a];
        const Vec3 seed = std::fabs(axisN.x) > 0.5f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
        const Vec3 U = normalize(seed - axisN * dot(seed, axisN));
        const Vec3 V = cross(axisN, U);
        for (int i = 0; i < S; ++i) {
            const float t = static_cast<float>(i) / S * kTwoPi;
            const Vec3 p = (U * std::cos(t) + V * std::sin(t)) * size;  // ring pt rel origin
            const float sx = dot(p, right) - mx, sy = dot(p, up) - my;
            const float d = std::sqrt(sx * sx + sy * sy);
            if (d < best) { best = d; picked = a; }
        }
    }
    return picked;
}

void drawRing(Renderer& r, Vec3 c, Vec3 axis, float radius, Vec3 color) {
    Vec3 seed = std::fabs(axis.x) > 0.5f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 u = normalize(seed - axis * dot(seed, axis));
    Vec3 v = cross(axis, u);
    const int N = 32;
    Vec3 prev = c + u * radius;
    for (int i = 1; i <= N; ++i) {
        const float t = static_cast<float>(i) / N * kTwoPi;
        const Vec3 p = c + (u * std::cos(t) + v * std::sin(t)) * radius;
        r.drawLineOverlayThick(prev, p, color);
        prev = p;
    }
}

void drawBox(Renderer& r, Vec3 c, float h, Vec3 color) {
    const Vec3 k[8] = {
        {c.x - h, c.y - h, c.z - h}, {c.x + h, c.y - h, c.z - h},
        {c.x + h, c.y + h, c.z - h}, {c.x - h, c.y + h, c.z - h},
        {c.x - h, c.y - h, c.z + h}, {c.x + h, c.y - h, c.z + h},
        {c.x + h, c.y + h, c.z + h}, {c.x - h, c.y + h, c.z + h},
    };
    const int e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (auto& pr : e) r.drawLineOverlayThick(k[pr[0]], k[pr[1]], color);
}

// A little pyramid arrowhead at the tip of a translate axis.
void drawArrowhead(Renderer& r, Vec3 tip, Vec3 dir, float size, Vec3 color) {
    Vec3 seed = std::fabs(dir.x) > 0.5f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 u = normalize(seed - dir * dot(seed, dir));
    Vec3 v = cross(dir, u);
    const float len = size * 0.18f;
    const float rad = size * 0.06f;
    const Vec3 base = tip - dir * len;
    const Vec3 b[4] = {base + u * rad, base + v * rad, base - u * rad, base - v * rad};
    for (int i = 0; i < 4; ++i) {
        r.drawLineOverlayThick(tip, b[i], color);          // sides to the point
        r.drawLineOverlayThick(b[i], b[(i + 1) % 4], color);  // base ring
    }
}

}  // namespace

void Gizmo::setMode(GizmoMode m) { if (axis_ < 0) mode_ = m; }
void Gizmo::setSpace(GizmoSpace s) { if (axis_ < 0) space_ = s; }
void Gizmo::toggleSpace() {
    if (axis_ < 0)
        space_ = (space_ == GizmoSpace::World) ? GizmoSpace::Local : GizmoSpace::World;
}

bool Gizmo::update(SceneEntity& e, Vec3 origin, const Ray& ray,
                   bool mousePressed, bool mouseDown, Vec3 camPos) {
    const float size = gizmoSize(camPos, origin);
    // Scale is inherently per-local-axis (matches Unreal/Unity, whose world/local
    // toggle is disabled for scale).
    const bool effectiveLocal = (mode_ == GizmoMode::Scale) || (space_ == GizmoSpace::Local);

    // Idle / pre-drag: orient handles by the entity's CURRENT rotation.
    Vec3 ax[3];
    buildBasis(effectiveLocal, e.rotation, ax);

    if (!mouseDown) {
        axis_ = -1;
        hoveredAxis_ = pickHandle(mode_, ray, origin, ax, camPos, size);  // hover feedback
        return false;
    }

    // Begin a drag on press over a handle.
    if (mousePressed && axis_ < 0) {
        const int picked = pickHandle(mode_, ray, origin, ax, camPos, size);
        hoveredAxis_ = picked;
        if (picked < 0) return false;   // empty press -> host re-selects
        axis_        = picked;
        startPos_    = e.position;
        startScale_  = e.scale;
        startRot_    = e.rotation;
        startOrigin_ = origin;
        if (picked == kCenterHandle) {        // center free-move: camera-facing plane
            startNormal_ = normalize(camPos - startOrigin_);
            Vec3 hit;
            startHit_ = rayPlane(ray, startOrigin_, startNormal_, hit) ? hit : startOrigin_;
        } else if (picked >= 3) {             // planar: the handle's oriented plane
            Vec3 hit;
            startHit_ = rayPlane(ray, startOrigin_, ax[picked - 3], hit) ? hit : startOrigin_;
        } else {
            float p = 0.0f;
            if (mode_ == GizmoMode::Rotate) screenAngle(ray, startOrigin_, camPos, p);
            else                            rayAxisParam(ray, startOrigin_, ax[picked], p);
            startParam_ = p;
            lastParam_  = p;
        }
        return true;
    }

    // Continue a drag. Freeze the basis to startRot_ so the handle frame is fixed
    // for the whole drag (a Rotate drag changes e.rotation every frame). On a
    // degenerate/near-parallel solve, hold the last value (no movement this frame).
    if (axis_ >= 0) {
        buildBasis(effectiveLocal, startRot_, ax);
        if (axis_ == kCenterHandle) {  // free-move in the camera-facing plane
            Vec3 hit;
            if (rayPlane(ray, startOrigin_, startNormal_, hit))
                e.position = startPos_ + (hit - startHit_);
            return true;
        }
        if (axis_ >= 3) {  // planar drag: move in the handle's oriented plane
            Vec3 hit;
            if (rayPlane(ray, startOrigin_, ax[axis_ - 3], hit))
                e.position = startPos_ + (hit - startHit_);
            return true;
        }
        const Vec3 dir = ax[axis_];
        if (mode_ == GizmoMode::Translate) {
            float p;
            if (rayAxisParam(ray, startOrigin_, dir, p)) lastParam_ = p; else p = lastParam_;
            e.position = startPos_ + dir * (p - startParam_);
        } else if (mode_ == GizmoMode::Scale) {
            float p;
            if (rayAxisParam(ray, startOrigin_, dir, p)) lastParam_ = p; else p = lastParam_;
            float s = (axis_ == 0 ? startScale_.x : axis_ == 1 ? startScale_.y : startScale_.z) + (p - startParam_);
            if (s < 0.01f) s = 0.01f;
            if (axis_ == 0) e.scale.x = s; else if (axis_ == 1) e.scale.y = s; else e.scale.z = s;
        } else {  // Rotate
            float ang;
            if (screenAngle(ray, startOrigin_, camPos, ang)) lastParam_ = ang; else ang = lastParam_;
            // The drag angle is measured CCW in the camera's screen plane (constant
            // speed, grabbable even edge-on). Flip the sign when the rotation axis
            // points away from the camera so the object follows the cursor's
            // on-screen direction. `dir` (= ax[axis_]) is frozen to startRot_, so
            // World mode still reduces to a world-axis rotation about that axis.
            const float facing = dot(dir, normalize(camPos - startOrigin_));
            const float s = (facing < 0.0f) ? -1.0f : 1.0f;
            e.rotation = Quat::fromAxisAngle(dir, s * (ang - startParam_)) * startRot_;
        }
        return true;
    }
    return false;
}

void Gizmo::draw(Renderer& renderer, Vec3 origin, Quat rotation, Vec3 camPos) const {
    const float size = gizmoSize(camPos, origin);
    const int highlight = (axis_ >= 0) ? axis_ : hoveredAxis_;
    const bool effectiveLocal = (mode_ == GizmoMode::Scale) || (space_ == GizmoSpace::Local);
    // Draw in the LIVE entity frame so the handles track the object during a
    // drag (a Local rotate spins the rings with the mesh for feedback). The
    // dragged rotate-ring's normal is invariant under its own rotation, so it
    // stays put while the other two follow. The rotation MATH in update() still
    // uses the frozen startRot_ basis for a stable axis + angle reference.
    Vec3 ax[3];
    buildBasis(effectiveLocal, rotation, ax);

    auto colorFor = [&](int id) -> Vec3 {
        Vec3 c;
        if (id == kCenterHandle) c = Vec3{0.85f, 0.85f, 0.85f};  // center: gray
        else if (id >= 3)        c = axisColor(id - 3);          // planar: its normal-axis color
        else                     c = axisColor(id);
        if (id == highlight) {  // brighten the hovered/active handle
            return Vec3{c.x + 0.4f > 1.0f ? 1.0f : c.x + 0.4f,
                        c.y + 0.4f > 1.0f ? 1.0f : c.y + 0.4f,
                        c.z + 0.4f > 1.0f ? 1.0f : c.z + 0.4f};
        }
        return c * 0.55f;  // dim the others
    };

    if (mode_ == GizmoMode::Rotate) {
        for (int a = 0; a < 3; ++a) drawRing(renderer, origin, ax[a], size, colorFor(a));
        return;
    }

    // Axis handles (translate arrows / scale boxes).
    for (int a = 0; a < 3; ++a) {
        const Vec3 tip = origin + ax[a] * size;
        renderer.drawLineOverlayThick(origin, tip, colorFor(a));
        if (mode_ == GizmoMode::Scale)          drawBox(renderer, tip, size * 0.08f, colorFor(a));
        else if (mode_ == GizmoMode::Translate) drawArrowhead(renderer, tip, ax[a], size, colorFor(a));
    }

    if (mode_ != GizmoMode::Translate) return;

    // Planar move handles: translucent filled quads at the inner corner, anchored
    // at the intersection (origin) — Unity-style.
    const float reach = kPlaneReach * size;
    for (int n = 0; n < 3; ++n) {
        Vec3 u, v;
        planeAxes(ax, n, u, v);
        const Vec3 c10 = origin + u * reach;
        const Vec3 c11 = origin + u * reach + v * reach;
        const Vec3 c01 = origin + v * reach;
        const Vec3 col = colorFor(3 + n);
        renderer.drawTriOverlay(origin, c10, c11, col);
        renderer.drawTriOverlay(origin, c11, c01, col);
        // Outline only the two OUTER edges (the L away from the origin).
        renderer.drawLineOverlayThick(c10, c11, col);
        renderer.drawLineOverlayThick(c01, c11, col);
    }

    // Center free-move handle: a translucent camera-facing quad at the origin,
    // with a gray outline so it reads against the scene.
    Vec3 right, up;
    cameraBasis(origin, camPos, right, up);
    const float h = size * kCenterHalf;
    const Vec3 q00 = origin - right * h - up * h;
    const Vec3 q10 = origin + right * h - up * h;
    const Vec3 q11 = origin + right * h + up * h;
    const Vec3 q01 = origin - right * h + up * h;
    const Vec3 cc = colorFor(kCenterHandle);
    renderer.drawTriOverlay(q00, q10, q11, cc);
    renderer.drawTriOverlay(q00, q11, q01, cc);
    renderer.drawLineOverlayThick(q00, q10, cc);
    renderer.drawLineOverlayThick(q10, q11, cc);
    renderer.drawLineOverlayThick(q11, q01, cc);
    renderer.drawLineOverlayThick(q01, q00, cc);
}

}  // namespace iron
