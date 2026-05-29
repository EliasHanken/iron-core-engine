#include "scene/Picking.h"

namespace iron {

Ray screenPointToRay(const Mat4& view, const Mat4& proj, Vec2 mousePx,
                     Vec2 viewportPx, Vec3 camPos) {
    const float ndcX = 2.0f * mousePx.x / viewportPx.x - 1.0f;
    const float ndcY = 1.0f - 2.0f * mousePx.y / viewportPx.y;  // GLFW y is top-down
    const Mat4 invVP = inverse(proj * view);
    const Vec4 farH = invVP * Vec4{ndcX, ndcY, 1.0f, 1.0f};      // far plane (NDC z=1)
    const Vec3 farWorld{farH.x / farH.w, farH.y / farH.w, farH.z / farH.w};
    Ray r;
    r.origin = camPos;
    r.direction = normalize(farWorld - camPos);
    return r;
}

int pickEntity(const Ray& ray, const std::vector<Aabb>& worldAabbs) {
    int best = -1;
    float bestT = 1e30f;
    for (int i = 0; i < static_cast<int>(worldAabbs.size()); ++i) {
        float t = 0.0f;
        if (intersectRayAabb(ray, worldAabbs[i], t) && t < bestT) {
            bestT = t;
            best = i;
        }
    }
    return best;
}

}  // namespace iron
