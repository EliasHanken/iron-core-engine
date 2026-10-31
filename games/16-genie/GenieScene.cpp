#include "GenieScene.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace genie {

using iron::MeshData;
using iron::Vec3;
using iron::Vertex;

namespace {

constexpr float kTau = 2.0f * std::numbers::pi_v<float>;

constexpr float kLampX = -5.6f;
constexpr float kLampZ = 0.4f;

// Spout tip in lamp-local units: the last ring of makeBody's profile.
constexpr Vec3 kSpoutTipLocal{0.98f, 0.40f, 0.0f};

// How high the lamp's origin sits above its lowest point (the foot).
constexpr float kFootDepth = 0.33f;

// Adds a closed skin through a stack of rings. Each ring is a circle of `radius`
// around `center`, perpendicular to `axis`. Normals are left for smoothNormals.
struct Ring {
    Vec3  center;
    float radius;
};

void appendLoft(MeshData& out, const std::vector<Ring>& rings, Vec3 axis, int sides) {
    const auto base = static_cast<std::uint32_t>(out.vertices.size());
    const Vec3 side = iron::normalize(
        iron::cross(axis, std::abs(axis.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
    const Vec3 lift = iron::cross(side, axis);

    for (std::size_t r = 0; r < rings.size(); ++r) {
        for (int s = 0; s <= sides; ++s) {
            const float a = kTau * static_cast<float>(s) / static_cast<float>(sides);
            Vertex v{};
            v.position = rings[r].center +
                         (side * std::cos(a) + lift * std::sin(a)) * rings[r].radius;
            v.uv = {static_cast<float>(s) / static_cast<float>(sides),
                    static_cast<float>(r) / static_cast<float>(rings.size() - 1)};
            v.tangent = axis;
            out.vertices.push_back(v);
        }
    }

    const auto stride = static_cast<std::uint32_t>(sides + 1);
    for (std::uint32_t r = 0; r + 1 < rings.size(); ++r) {
        for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(sides); ++s) {
            const std::uint32_t a = base + r * stride + s;
            const std::uint32_t b = a + stride;
            out.indices.insert(out.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }
}

// Area-weighted vertex normals, flipped outward from `center` so a loft's winding
// never matters. The lamp's shapes are all convex enough for that to hold.
void smoothNormals(MeshData& mesh, std::size_t firstVertex, std::size_t firstIndex, Vec3 center) {
    for (std::size_t i = firstVertex; i < mesh.vertices.size(); ++i) {
        mesh.vertices[i].normal = {0, 0, 0};
    }
    for (std::size_t i = firstIndex; i + 2 < mesh.indices.size(); i += 3) {
        Vertex& a = mesh.vertices[mesh.indices[i]];
        Vertex& b = mesh.vertices[mesh.indices[i + 1]];
        Vertex& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 n = iron::cross(b.position - a.position, c.position - a.position);
        a.normal = a.normal + n;
        b.normal = b.normal + n;
        c.normal = c.normal + n;
    }
    for (std::size_t i = firstVertex; i < mesh.vertices.size(); ++i) {
        Vertex& v = mesh.vertices[i];
        if (iron::dot(v.normal, v.normal) < 1e-12f) {
            v.normal = {0, 1, 0};
            continue;
        }
        v.normal = iron::normalize(v.normal);
    }

    float outward = 0.0f;
    for (std::size_t i = firstVertex; i < mesh.vertices.size(); ++i) {
        outward += iron::dot(mesh.vertices[i].normal, mesh.vertices[i].position - center);
    }
    if (outward >= 0.0f) return;

    for (std::size_t i = firstVertex; i < mesh.vertices.size(); ++i) {
        mesh.vertices[i].normal = -mesh.vertices[i].normal;
    }
    for (std::size_t i = firstIndex; i + 2 < mesh.indices.size(); i += 3) {
        std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
    }
}

// An ear-shaped loop leaving the belly's top-back and rejoining it lower down.
// It lies in the z = 0 plane, so each ring is built from the curve's outward
// direction and +Z and can never twist.
void appendHandle(MeshData& mesh) {
    constexpr int kSteps = 36;
    constexpr int kSides = 14;
    constexpr float kRadius = 0.032f;
    constexpr float kCenterX = -0.62f;
    constexpr float kRadiusX = 0.34f;
    constexpr float kRadiusY = 0.30f;
    constexpr float kStartAngle = 45.0f * kTau / 360.0f;
    constexpr float kEndAngle = 315.0f * kTau / 360.0f;

    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (int i = 0; i <= kSteps; ++i) {
        const float angle = kStartAngle +
            (kEndAngle - kStartAngle) * static_cast<float>(i) / kSteps;
        const Vec3 center{kCenterX + kRadiusX * std::cos(angle),
                          kRadiusY * std::sin(angle), 0.0f};
        const Vec3 outward = iron::normalize(
            Vec3{std::cos(angle) / kRadiusX, std::sin(angle) / kRadiusY, 0.0f});

        for (int s = 0; s <= kSides; ++s) {
            const float a = kTau * static_cast<float>(s) / kSides;
            Vertex v{};
            v.normal = outward * std::cos(a) + Vec3{0, 0, 1} * std::sin(a);
            v.position = center + v.normal * kRadius;
            v.uv = {static_cast<float>(s) / kSides, static_cast<float>(i) / kSteps};
            v.tangent = iron::normalize(Vec3{-outward.y, outward.x, 0.0f});
            mesh.vertices.push_back(v);
        }
    }

    const std::uint32_t stride = kSides + 1;
    for (std::uint32_t i = 0; i < kSteps; ++i) {
        for (std::uint32_t s = 0; s < kSides; ++s) {
            const std::uint32_t a = base + i * stride + s;
            const std::uint32_t b = a + stride;
            mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
}

// Catmull-Rom through the profile, so a dozen hand-placed rings give a smooth
// silhouette even in the opening close-up.
std::vector<Ring> smoothProfile(const std::vector<Ring>& rings) {
    constexpr int kSubdivisions = 6;

    const auto catmullRom = [](float a, float b, float c, float d, float t) {
        return 0.5f * (2.0f * b + (c - a) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t * t +
                       (3.0f * b - a - 3.0f * c + d) * t * t * t);
    };

    std::vector<Ring> out;
    const auto last = static_cast<int>(rings.size()) - 1;
    for (int i = 0; i < last; ++i) {
        const Ring& a = rings[static_cast<std::size_t>(std::max(i - 1, 0))];
        const Ring& b = rings[static_cast<std::size_t>(i)];
        const Ring& c = rings[static_cast<std::size_t>(i + 1)];
        const Ring& d = rings[static_cast<std::size_t>(std::min(i + 2, last))];
        for (int s = 0; s < kSubdivisions; ++s) {
            const float t = static_cast<float>(s) / kSubdivisions;
            out.push_back({{catmullRom(a.center.x, b.center.x, c.center.x, d.center.x, t),
                            catmullRom(a.center.y, b.center.y, c.center.y, d.center.y, t),
                            catmullRom(a.center.z, b.center.z, c.center.z, d.center.z, t)},
                           std::max(catmullRom(a.radius, b.radius, c.radius, d.radius, t), 0.0f)});
        }
    }
    out.push_back(rings.back());
    return out;
}

void addPart(MeshData& mesh, const std::vector<Ring>& rings, Vec3 axis, Vec3 center) {
    const std::size_t firstVertex = mesh.vertices.size();
    const std::size_t firstIndex = mesh.indices.size();
    appendLoft(mesh, smoothProfile(rings), axis, 48);
    smoothNormals(mesh, firstVertex, firstIndex, center);
}

}  // namespace

float groundHeight(float x, float z) {
    return 0.70f * std::sin(0.21f * x + 0.6f * std::sin(0.13f * z)) +
           0.35f * std::sin(0.37f * z + 0.19f * x + 1.3f) +
           0.12f * std::sin(0.83f * x - 0.47f * z);
}

Vec3 lampPosition() {
    return {kLampX, groundHeight(kLampX, kLampZ) + kFootDepth * kLampScale - 0.08f, kLampZ};
}

Vec3 spoutTip() {
    return lampPosition() + kSpoutTipLocal * kLampScale;
}

MeshData makeDunes() {
    constexpr int kCells = 420;
    constexpr float kSizeX = 150.0f;
    constexpr float kSizeZ = 110.0f;
    constexpr Vec3 kCenter{0.0f, 0.0f, -22.0f};
    constexpr float kNormalStep = 0.05f;

    MeshData mesh;
    mesh.vertices.reserve((kCells + 1) * (kCells + 1));
    mesh.indices.reserve(kCells * kCells * 6);

    for (int row = 0; row <= kCells; ++row) {
        for (int col = 0; col <= kCells; ++col) {
            const float u = static_cast<float>(col) / kCells;
            const float v = static_cast<float>(row) / kCells;
            const float x = kCenter.x + (u - 0.5f) * kSizeX;
            const float z = kCenter.z + (v - 0.5f) * kSizeZ;

            const float dx = groundHeight(x + kNormalStep, z) - groundHeight(x - kNormalStep, z);
            const float dz = groundHeight(x, z + kNormalStep) - groundHeight(x, z - kNormalStep);

            Vertex vertex{};
            vertex.position = {x, groundHeight(x, z), z};
            vertex.normal = iron::normalize(Vec3{-dx, 2.0f * kNormalStep, -dz});
            vertex.uv = {u, v};
            vertex.tangent = iron::normalize(Vec3{2.0f * kNormalStep, dx, 0.0f});
            mesh.vertices.push_back(vertex);
        }
    }

    const std::uint32_t stride = kCells + 1;
    for (std::uint32_t row = 0; row < kCells; ++row) {
        for (std::uint32_t col = 0; col < kCells; ++col) {
            const std::uint32_t a = row * stride + col;
            const std::uint32_t b = a + stride;
            mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return mesh;
}

MeshData makeLamp() {
    MeshData mesh;

    // Body and spout: one skin along +X, the belly narrowing and rising into the
    // tip. The two-point caps close each end.
    const std::vector<Ring> body = {
        {{-0.62f, 0.00f, 0}, 0.000f}, {{-0.61f, 0.00f, 0}, 0.060f}, {{-0.56f, 0.00f, 0}, 0.150f},
        {{-0.44f, 0.00f, 0}, 0.240f}, {{-0.24f, 0.00f, 0}, 0.310f}, {{ 0.00f, 0.00f, 0}, 0.325f},
        {{ 0.20f, 0.00f, 0}, 0.285f}, {{ 0.38f, 0.02f, 0}, 0.215f}, {{ 0.52f, 0.06f, 0}, 0.150f},
        {{ 0.66f, 0.13f, 0}, 0.100f}, {{ 0.78f, 0.22f, 0}, 0.075f}, {{ 0.88f, 0.31f, 0}, 0.062f},
        {{ 0.96f, 0.385f, 0}, 0.058f}, {{ kSpoutTipLocal.x, kSpoutTipLocal.y, 0}, 0.070f},
        {{ kSpoutTipLocal.x, kSpoutTipLocal.y, 0}, 0.000f},
    };
    addPart(mesh, body, {1, 0, 0}, {0, 0, 0});

    const std::vector<Ring> foot = {
        {{0, -0.33f, 0}, 0.000f}, {{0, -0.33f, 0}, 0.210f}, {{0, -0.31f, 0}, 0.215f},
        {{0, -0.29f, 0}, 0.150f}, {{0, -0.25f, 0}, 0.115f}, {{0, -0.19f, 0}, 0.135f},
    };
    addPart(mesh, foot, {0, 1, 0}, {0, -0.3f, 0});

    const std::vector<Ring> lid = {
        {{0, 0.30f, 0}, 0.195f}, {{0, 0.35f, 0}, 0.200f}, {{0, 0.37f, 0}, 0.165f},
        {{0, 0.43f, 0}, 0.115f}, {{0, 0.49f, 0}, 0.050f}, {{0, 0.52f, 0}, 0.030f},
        {{0, 0.55f, 0}, 0.055f}, {{0, 0.59f, 0}, 0.055f}, {{0, 0.63f, 0}, 0.022f},
        {{0, 0.66f, 0}, 0.000f},
    };
    addPart(mesh, lid, {0, 1, 0}, {0, 0.42f, 0});

    appendHandle(mesh);
    return mesh;
}

std::vector<std::uint8_t> makeSandNormalMap(int size) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size) * size * 4);

    // Whole wave counts keep every term tileable. The slow warp bends the ridges
    // so they read as wind ripples rather than stripes.
    const auto height = [](float u, float v) {
        const float warp = 0.05f * std::sin(kTau * (2.0f * u + v)) +
                           0.03f * std::sin(kTau * (3.0f * u - 2.0f * v));
        return std::sin(kTau * 9.0f * (v + warp)) +
               0.4f * std::sin(kTau * (17.0f * v + 3.0f * u + 2.0f * warp));
    };

    constexpr float kStep = 0.001f;
    constexpr float kStrength = 0.016f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(size);
            const float v = static_cast<float>(y) / static_cast<float>(size);
            const float du = (height(u + kStep, v) - height(u - kStep, v)) * kStrength / kStep;
            const float dv = (height(u, v + kStep) - height(u, v - kStep)) * kStrength / kStep;
            const Vec3 n = iron::normalize(Vec3{-du, -dv, 1.0f});

            const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
            rgba[i + 0] = static_cast<std::uint8_t>((n.x * 0.5f + 0.5f) * 255.0f);
            rgba[i + 1] = static_cast<std::uint8_t>((n.y * 0.5f + 0.5f) * 255.0f);
            rgba[i + 2] = static_cast<std::uint8_t>((n.z * 0.5f + 0.5f) * 255.0f);
            rgba[i + 3] = 255;
        }
    }
    return rgba;
}

std::array<std::vector<std::uint8_t>, 6> makeNightSkyFaces(int size) {
    std::array<std::vector<std::uint8_t>, 6> faces;

    for (int face = 0; face < 6; ++face) {
        auto& pixels = faces[static_cast<std::size_t>(face)];
        pixels.resize(static_cast<std::size_t>(size) * size * 4);

        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float s = 2.0f * (static_cast<float>(x) + 0.5f) / size - 1.0f;
                const float t = 2.0f * (static_cast<float>(y) + 0.5f) / size - 1.0f;

                Vec3 dir{};
                switch (face) {
                    case 0: dir = { 1, -t, -s}; break;
                    case 1: dir = {-1, -t,  s}; break;
                    case 2: dir = { s,  1,  t}; break;
                    case 3: dir = { s, -1, -t}; break;
                    case 4: dir = { s, -t,  1}; break;
                    default: dir = {-s, -t, -1}; break;
                }
                dir = iron::normalize(dir);

                const float horizon = std::exp(-std::abs(dir.y) * 5.0f);
                const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
                pixels[i + 0] = static_cast<std::uint8_t>(5.0f + 26.0f * horizon);
                pixels[i + 1] = static_cast<std::uint8_t>(3.0f + 8.0f * horizon);
                pixels[i + 2] = static_cast<std::uint8_t>(9.0f + 34.0f * horizon);
                pixels[i + 3] = 255;
            }
        }
    }
    return faces;
}

}  // namespace genie
