#include "editor/Gizmo.h"

#include "render/Renderer.h"
#include "scene/SceneFormat.h"

#include <cmath>

namespace iron {

namespace {

constexpr float kGizmoScreenScale = 0.15f;  // gizmo world size ~= this * camera distance
constexpr float kHandlePickFrac   = 0.18f;  // pick threshold as a fraction of gizmo size
constexpr float kTwoPi            = 6.28318530718f;

Vec3 axisDir(int a)   { return a == 0 ? Vec3{1, 0, 0} : a == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1}; }
Vec3 axisColor(int a) { return a == 0 ? Vec3{1.0f, 0.25f, 0.25f}
                             : a == 1 ? Vec3{0.25f, 1.0f, 0.25f}
                                      : Vec3{0.35f, 0.45f, 1.0f}; }

float gizmoSize(Vec3 camPos, Vec3 origin) {
    const float d = length(camPos - origin) * kGizmoScreenScale;
    return d < 0.001f ? 0.001f : d;
}

// Closest distance between two segments (Ericson, Real-Time Collision Detection
// 5.1.9). Robust under all the parallel / clamped cases.
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
        // both segments degenerate to points
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

// Closest distance between a forward ray and the segment [a,b]. The ray is
// treated as a long segment so only t >= 0 along it is considered.
float raySegmentDistance(const Ray& ray, Vec3 a, Vec3 b) {
    return segSegDistance(ray.origin, ray.origin + ray.direction * 1.0e4f, a, b);
}

// Parameter p along the infinite line (origin + dir*p, dir unit) of the point
// closest to the ray.
float rayAxisParam(const Ray& ray, Vec3 origin, Vec3 dir) {
    const Vec3 r = ray.origin - origin;
    const float b = dot(ray.direction, dir);
    const float c = dot(ray.direction, r);
    const float f = dot(dir, r);
    const float denom = 1.0f - b * b;
    if (denom < 1e-6f) return 0.0f;    // near-parallel
    return (f - b * c) / denom;
}

// Intersect ray with the plane through `origin` with unit normal `n`.
bool rayPlane(const Ray& ray, Vec3 origin, Vec3 n, Vec3& hit) {
    const float denom = dot(ray.direction, n);
    if (std::fabs(denom) < 1e-6f) return false;
    const float t = dot(origin - ray.origin, n) / denom;
    if (t < 0.0f) return false;
    hit = ray.origin + ray.direction * t;
    return true;
}

// Signed angle of the ray's hit point on the rotation plane of `axis`, about the
// entity origin. Uses a fixed per-axis 2D basis so the angle is consistent across
// frames of one drag.
float ringAngle(const Ray& ray, Vec3 origin, int axis) {
    Vec3 hit;
    if (!rayPlane(ray, origin, axisDir(axis), hit)) return 0.0f;
    Vec3 u, v;
    if (axis == 0)      { u = Vec3{0, 1, 0}; v = Vec3{0, 0, 1}; }
    else if (axis == 1) { u = Vec3{0, 0, 1}; v = Vec3{1, 0, 0}; }
    else                { u = Vec3{1, 0, 0}; v = Vec3{0, 1, 0}; }
    const Vec3 d = hit - origin;
    return std::atan2(dot(d, v), dot(d, u));
}

void drawRing(Renderer& r, Vec3 c, Vec3 axis, float radius, Vec3 color) {
    Vec3 seed = std::fabs(axis.x) > 0.5f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 u = normalize(seed - axis * dot(seed, axis));  // perpendicular to axis
    Vec3 v = cross(axis, u);
    const int N = 32;
    Vec3 prev = c + u * radius;
    for (int i = 1; i <= N; ++i) {
        const float t = static_cast<float>(i) / N * kTwoPi;
        const Vec3 p = c + (u * std::cos(t) + v * std::sin(t)) * radius;
        r.drawLine(prev, p, color);
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
    for (auto& pr : e) r.drawLine(k[pr[0]], k[pr[1]], color);
}

}  // namespace

void Gizmo::setMode(GizmoMode m) { if (axis_ < 0) mode_ = m; }

bool Gizmo::update(SceneEntity& e, const Ray& ray, bool mousePressed,
                   bool mouseDown, Vec3 camPos) {
    const Vec3 origin = e.position;
    const float size = gizmoSize(camPos, origin);

    if (!mouseDown) { axis_ = -1; return false; }

    // Begin a drag on press if a handle is under the ray.
    if (mousePressed && axis_ < 0) {
        int picked = -1;
        if (mode_ == GizmoMode::Translate || mode_ == GizmoMode::Scale) {
            float best = size * kHandlePickFrac;
            for (int a = 0; a < 3; ++a) {
                const float d = raySegmentDistance(ray, origin, origin + axisDir(a) * size);
                if (d < best) { best = d; picked = a; }
            }
        } else {  // Rotate
            float best = size * kHandlePickFrac;
            for (int a = 0; a < 3; ++a) {
                Vec3 hit;
                if (rayPlane(ray, origin, axisDir(a), hit)) {
                    const float err = std::fabs(length(hit - origin) - size);
                    if (err < best) { best = err; picked = a; }
                }
            }
        }
        if (picked < 0) return false;   // nothing grabbed -> let the host re-select
        axis_       = picked;
        startPos_   = e.position;
        startScale_ = e.scale;
        startRot_   = e.rotation;
        startParam_ = (mode_ == GizmoMode::Rotate)
                          ? ringAngle(ray, origin, picked)
                          : rayAxisParam(ray, origin, axisDir(picked));
        return true;
    }

    // Continue a drag.
    if (axis_ >= 0) {
        const Vec3 dir = axisDir(axis_);
        if (mode_ == GizmoMode::Translate) {
            const float p = rayAxisParam(ray, startPos_, dir);
            e.position = startPos_ + dir * (p - startParam_);
        } else if (mode_ == GizmoMode::Scale) {
            const float p = rayAxisParam(ray, startPos_, dir);
            const float delta = p - startParam_;
            float s = (axis_ == 0 ? startScale_.x : axis_ == 1 ? startScale_.y : startScale_.z) + delta;
            if (s < 0.01f) s = 0.01f;
            if (axis_ == 0) e.scale.x = s; else if (axis_ == 1) e.scale.y = s; else e.scale.z = s;
        } else {  // Rotate
            const float angle = ringAngle(ray, startPos_, axis_);
            e.rotation = Quat::fromAxisAngle(dir, angle - startParam_) * startRot_;
        }
        return true;
    }
    return false;
}

void Gizmo::draw(Renderer& renderer, const SceneEntity& e, Vec3 camPos) const {
    const Vec3 o = e.position;
    const float size = gizmoSize(camPos, o);
    if (mode_ == GizmoMode::Rotate) {
        for (int a = 0; a < 3; ++a) drawRing(renderer, o, axisDir(a), size, axisColor(a));
        return;
    }
    for (int a = 0; a < 3; ++a) {
        const Vec3 tip = o + axisDir(a) * size;
        renderer.drawLine(o, tip, axisColor(a));
        if (mode_ == GizmoMode::Scale) drawBox(renderer, tip, size * 0.08f, axisColor(a));
    }
}

}  // namespace iron
