#include "scene/Mesh.h"

#include "math/Aabb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace iron {

void appendBox(MeshData& out, Vec3 center, Vec3 size) {
    // Six faces, each a quad of 4 vertices with a shared outward normal.
    // Corner components are +/-0.5 (a unit cube); scaled by `size` and
    // shifted by `center` they span center +/- size/2. Winding is
    // counter-clockwise seen from outside.
    struct Face {
        Vec3 normal;
        Vec3 corners[4];
    };

    const Face faces[6] = {
        {{ 1, 0, 0}, {{ 0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f,-0.5f}}},
        {{-1, 0, 0}, {{-0.5f, 0.5f, 0.5f},{-0.5f, 0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f,-0.5f, 0.5f}}},
        {{ 0, 1, 0}, {{-0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f,-0.5f},{-0.5f, 0.5f,-0.5f}}},
        {{ 0,-1, 0}, {{-0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f, 0.5f},{-0.5f,-0.5f, 0.5f}}},
        {{ 0, 0, 1}, {{-0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f}}},
        {{ 0, 0,-1}, {{ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f}}},
    };

    // Per-face UV extents in world units: U and V span the face's two
    // in-plane dimensions so textures tile rather than stretch.
    // Face order matches the `faces` array above:
    //   0(+X),1(-X): in-plane axes are Z and Y → (size.z, size.y)
    //   2(+Y),3(-Y): in-plane axes are X and Z → (size.x, size.z)
    //   4(+Z),5(-Z): in-plane axes are X and Y → (size.x, size.y)
    const Vec2 faceExtents[6] = {
        {size.z, size.y},  // +X
        {size.z, size.y},  // -X
        {size.x, size.z},  // +Y
        {size.x, size.z},  // -Y
        {size.x, size.y},  // +Z
        {size.x, size.y},  // -Z
    };

    // Per-face tangent: the world-space direction in which U increases.
    // Derived by inspecting the corner winding and UV assignment above:
    //   +X: corners vary in Z, UV[0..1] go 0..uExt as z goes +→- ... wait,
    //       corner[0]=(z=-0.5), corner[1]=(z=+0.5) → U increases with +Z.
    //   -X: corner[0]=(z=+0.5), corner[1]=(z=-0.5) → U increases with -Z.
    //   +Y: corner[0]=(x=-0.5), corner[1]=(x=+0.5) → U increases with +X.
    //   -Y: corner[0]=(x=-0.5), corner[1]=(x=+0.5) → U increases with +X.
    //   +Z: corner[0]=(x=-0.5), corner[1]=(x=+0.5) → U increases with +X.
    //   -Z: corner[0]=(x=+0.5), corner[1]=(x=-0.5) → U increases with -X.
    const Vec3 faceTangents[6] = {
        { 0.0f, 0.0f,  1.0f},  // +X: U axis is +Z
        { 0.0f, 0.0f, -1.0f},  // -X: U axis is -Z
        { 1.0f, 0.0f,  0.0f},  // +Y: U axis is +X
        { 1.0f, 0.0f,  0.0f},  // -Y: U axis is +X
        { 1.0f, 0.0f,  0.0f},  // +Z: U axis is +X
        {-1.0f, 0.0f,  0.0f},  // -Z: U axis is -X
    };

    for (int fi = 0; fi < 6; ++fi) {
        const Face& face = faces[fi];
        const float uExt = faceExtents[fi].x;
        const float vExt = faceExtents[fi].y;
        const Vec2 uvs[4] = {{0.0f, 0.0f}, {uExt, 0.0f}, {uExt, vExt}, {0.0f, vExt}};
        const Vec3 tangent = faceTangents[fi];

        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        for (int i = 0; i < 4; ++i) {
            const Vec3 c = face.corners[i];
            const Vec3 position{
                center.x + c.x * size.x,
                center.y + c.y * size.y,
                center.z + c.z * size.z,
            };
            out.vertices.push_back(Vertex{position, face.normal, uvs[i], tangent});
        }
        // Two triangles per quad: (0,1,2) and (0,2,3).
        out.indices.push_back(base + 0);
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 2);
        out.indices.push_back(base + 0);
        out.indices.push_back(base + 2);
        out.indices.push_back(base + 3);
    }
}

void appendTube(MeshData& out, const std::vector<Vec3>& points, float radius,
                int sides) {
    const int pointCount = static_cast<int>(points.size());
    if (pointCount < 2 || sides < 3 || radius <= 0.0f) {
        return;
    }

    constexpr float kTwoPi = 6.28318530717958647692f;
    const auto base = static_cast<std::uint32_t>(out.vertices.size());
    // Each ring has sides + 1 vertices: the last duplicates the first
    // position but carries U = 1, so the wrap-around quad's texture does not
    // run backwards across the seam.
    const int ringVertexCount = sides + 1;

    // --- ring vertices ---
    float vCoord = 0.0f;
    for (int i = 0; i < pointCount; ++i) {
        // Local rope direction (forward difference; backward at the last point).
        Vec3 dir = (i + 1 < pointCount) ? points[i + 1] - points[i]
                                        : points[i] - points[i - 1];
        const float dirLen = length(dir);
        dir = (dirLen > 1e-6f) ? dir * (1.0f / dirLen) : Vec3{0.0f, 0.0f, 1.0f};

        // A perpendicular frame. Use world up unless the rope is near-vertical.
        const Vec3 up = (std::fabs(dir.y) > 0.99f) ? Vec3{1.0f, 0.0f, 0.0f}
                                                   : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 right = normalize(cross(dir, up));
        const Vec3 ringUp = normalize(cross(right, dir));

        // V advances with arc length so the texture tiles down the rope.
        if (i > 0) {
            vCoord += length(points[i] - points[i - 1]) / (2.0f * radius);
        }

        for (int s = 0; s <= sides; ++s) {
            const float angle = kTwoPi * static_cast<float>(s)
                                       / static_cast<float>(sides);
            const Vec3 offset = right * (std::cos(angle) * radius)
                              + ringUp * (std::sin(angle) * radius);
            Vertex vert;
            vert.position = points[i] + offset;
            vert.normal = normalize(offset);  // radially outward
            vert.uv = Vec2{static_cast<float>(s) / static_cast<float>(sides),
                           vCoord};
            // Along-length direction. NOTE: for a tube UV layout where U
            // wraps around the ring and V tiles along length, the strict
            // TBN tangent is the circumferential direction; `dir` is the
            // bitangent. Acceptable today because ropes don't sample a
            // normal map; revisit when they do.
            vert.tangent = dir;
            out.vertices.push_back(vert);
        }
    }

    // --- stitch consecutive rings (CCW seen from outside) ---
    for (int i = 0; i + 1 < pointCount; ++i) {
        const auto ring0 =
            base + static_cast<std::uint32_t>(i * ringVertexCount);
        const auto ring1 =
            base + static_cast<std::uint32_t>((i + 1) * ringVertexCount);
        for (int s = 0; s < sides; ++s) {
            const auto s0 = static_cast<std::uint32_t>(s);
            const auto s1 = static_cast<std::uint32_t>(s + 1);
            out.indices.push_back(ring0 + s0);
            out.indices.push_back(ring1 + s0);
            out.indices.push_back(ring1 + s1);
            out.indices.push_back(ring0 + s0);
            out.indices.push_back(ring1 + s1);
            out.indices.push_back(ring0 + s1);
        }
    }
}

// A unit cube centered at the origin (side length 1) — built from appendBox.
MeshData makeCube() {
    MeshData data;
    appendBox(data, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});
    return data;
}

void appendQuad(MeshData& out, Vec3 center, Vec2 size, Vec3 normal) {
    // Pick an in-plane "u" axis. Try +X first (most common for ground
    // planes); if normal is parallel to +X, fall back to +Y. Project
    // the hint into the plane and normalise.
    Vec3 uHint = (std::fabs(normal.x) < 0.99f) ? Vec3{1.0f, 0.0f, 0.0f}
                                               : Vec3{0.0f, 1.0f, 0.0f};
    const float along = dot(uHint, normal);
    Vec3 u = uHint - normal * along;
    const float ulen = std::sqrt(dot(u, u));
    u = (ulen > 1e-6f) ? u * (1.0f / ulen) : Vec3{1.0f, 0.0f, 0.0f};
    // v is in-plane, perpendicular to u; (u, v, normal) is right-handed.
    Vec3 v = cross(normal, u);

    const float hx = size.x * 0.5f;
    const float hy = size.y * 0.5f;

    // Four corners, CCW seen from +normal: -u-v, +u-v, +u+v, -u+v
    Vec3 p0 = center + u * (-hx) + v * (-hy);
    Vec3 p1 = center + u * ( hx) + v * (-hy);
    Vec3 p2 = center + u * ( hx) + v * ( hy);
    Vec3 p3 = center + u * (-hx) + v * ( hy);

    const std::uint32_t base = static_cast<std::uint32_t>(out.vertices.size());

    out.vertices.push_back(Vertex{p0, normal, Vec2{0.0f,   0.0f},   u});
    out.vertices.push_back(Vertex{p1, normal, Vec2{size.x, 0.0f},   u});
    out.vertices.push_back(Vertex{p2, normal, Vec2{size.x, size.y}, u});
    out.vertices.push_back(Vertex{p3, normal, Vec2{0.0f,   size.y}, u});

    // Two triangles: (0, 1, 2) and (0, 2, 3).
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + 3);
}

Aabb meshBounds(const MeshData& mesh) {
    if (mesh.vertices.empty()) return Aabb{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}};
    Vec3 lo = mesh.vertices[0].position;
    Vec3 hi = lo;
    for (const Vertex& v : mesh.vertices) {
        lo.x = std::min(lo.x, v.position.x);
        lo.y = std::min(lo.y, v.position.y);
        lo.z = std::min(lo.z, v.position.z);
        hi.x = std::max(hi.x, v.position.x);
        hi.y = std::max(hi.y, v.position.y);
        hi.z = std::max(hi.z, v.position.z);
    }
    return Aabb{lo, hi};
}

} // namespace iron
