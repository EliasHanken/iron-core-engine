#include "asset/Ik.h"

#include <algorithm>
#include <cmath>

namespace iron {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

Quat rotationFromTo(Vec3 from, Vec3 to) {
    from = normalize(from);
    to = normalize(to);
    const float d = dot(from, to);
    if (d >= 1.0f - 1e-6f) return Quat::identity();
    if (d <= -1.0f + 1e-6f) {
        Vec3 axis = cross(Vec3{1, 0, 0}, from);
        if (length(axis) < 1e-4f) axis = cross(Vec3{0, 1, 0}, from);
        return Quat::fromAxisAngle(normalize(axis), kPi);
    }
    const Vec3 axis = normalize(cross(from, to));
    return Quat::fromAxisAngle(axis, std::acos(d));
}

TwoBoneIKResult solveTwoBoneIK(Vec3 root, Vec3 mid, Vec3 end,
                               Vec3 target, Vec3 pole) {
    const float eps = 1e-5f;
    const float lab = length(mid - root);  // root -> mid bone length
    const float lcb = length(end - mid);   // mid -> end bone length

    // Direction to target and clamped reach.
    const Vec3 toTarget = target - root;
    float lat = length(toTarget);
    const Vec3 dirAT = (lat > eps) ? toTarget * (1.0f / lat) : Vec3{1, 0, 0};
    const float minReach = std::fabs(lab - lcb) + eps;
    const float maxReach = lab + lcb - eps;
    lat = std::clamp(lat, minReach, maxReach);

    // New end position (== target when reachable).
    const Vec3 endPos = root + dirAT * lat;

    // Angle at the root between (root->mid) and (root->end), law of cosines.
    float cosRoot = (lab * lab + lat * lat - lcb * lcb) / (2.0f * lab * lat);
    cosRoot = std::clamp(cosRoot, -1.0f, 1.0f);
    const float rootAngle = std::acos(cosRoot);

    // Bend direction: component of (pole-root) perpendicular to dirAT.
    Vec3 bend = (pole - root) - dirAT * dot(pole - root, dirAT);
    if (length(bend) < eps) {
        bend = cross(dirAT, Vec3{0, 1, 0});
        if (length(bend) < eps) bend = cross(dirAT, Vec3{1, 0, 0});
    }
    bend = normalize(bend);

    // Mid position in the (dirAT, bend) plane.
    const Vec3 midPos = root
        + dirAT * (lab * std::cos(rootAngle))
        + bend * (lab * std::sin(rootAngle));

    // World-space rotation deltas so the bones orient along the new segments.
    const Quat rootDelta = rotationFromTo(mid - root, midPos - root);
    const Vec3 carriedMidEnd = rootDelta.rotate(end - mid);
    const Quat midDelta = rotationFromTo(carriedMidEnd, endPos - midPos);

    return TwoBoneIKResult{midPos, endPos, rootDelta, midDelta};
}

}  // namespace iron
