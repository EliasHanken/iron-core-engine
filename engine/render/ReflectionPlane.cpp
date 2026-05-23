#include "render/ReflectionPlane.h"

namespace iron {

Mat4 reflectionMatrix(const ReflectionPlane& plane) {
    // The 3x3 mirror block is I - 2 * (n n^T). The translation column is
    // 2 * d * n: this shifts the mirror so the plane passes through the
    // point d * n instead of the origin.
    const float nx = plane.normal.x;
    const float ny = plane.normal.y;
    const float nz = plane.normal.z;

    Mat4 m = {};  // zero-init: we fill every non-zero entry explicitly below
    m.at(0, 0) = 1.0f - 2.0f * nx * nx;
    m.at(0, 1) = -2.0f * nx * ny;
    m.at(0, 2) = -2.0f * nx * nz;
    m.at(0, 3) = 2.0f * plane.d * nx;

    m.at(1, 0) = -2.0f * ny * nx;
    m.at(1, 1) = 1.0f - 2.0f * ny * ny;
    m.at(1, 2) = -2.0f * ny * nz;
    m.at(1, 3) = 2.0f * plane.d * ny;

    m.at(2, 0) = -2.0f * nz * nx;
    m.at(2, 1) = -2.0f * nz * ny;
    m.at(2, 2) = 1.0f - 2.0f * nz * nz;
    m.at(2, 3) = 2.0f * plane.d * nz;

    m.at(3, 3) = 1.0f;
    return m;
}

} // namespace iron
