#include "IronScene.h"
#include "IronTimeline.h"

#include "ui/FontAtlas.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <random>
#include <utility>

namespace ironcore {

using iron::MeshData;
using iron::Vec3;
using iron::Vertex;

namespace {

constexpr float kTau = 2.0f * std::numbers::pi_v<float>;

// The tilt of the final rings, so they read as a disc from the camera.
constexpr float kRingTilt = 0.32f;

float hash3(int x, int y, int z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^
                      static_cast<std::uint32_t>(y) * 19349663u ^
                      static_cast<std::uint32_t>(z) * 83492791u ^ seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 16777215.0f;
}

float valueNoise(Vec3 p, std::uint32_t seed) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int x = static_cast<int>(fx), y = static_cast<int>(fy), z = static_cast<int>(fz);
    const auto smooth = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const float u = smooth(p.x - fx), v = smooth(p.y - fy), w = smooth(p.z - fz);
    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };

    return lerp(lerp(lerp(hash3(x, y, z, seed), hash3(x + 1, y, z, seed), u),
                     lerp(hash3(x, y + 1, z, seed), hash3(x + 1, y + 1, z, seed), u), v),
                lerp(lerp(hash3(x, y, z + 1, seed), hash3(x + 1, y, z + 1, seed), u),
                     lerp(hash3(x, y + 1, z + 1, seed), hash3(x + 1, y + 1, z + 1, seed), u), v),
                w);
}

float fbm(Vec3 p, std::uint32_t seed, int octaves) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    for (int i = 0; i < octaves; ++i) {
        sum += valueNoise(p, seed + static_cast<std::uint32_t>(i) * 131u) * amplitude;
        p = p * 2.03f;
        amplitude *= 0.5f;
    }
    return sum;
}

struct Icosphere {
    std::vector<Vec3> points;
    std::vector<std::uint32_t> triangles;
};

Icosphere makeIcosphere(int subdivisions) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    Icosphere sphere;
    for (Vec3 p : {Vec3{-1, t, 0}, Vec3{1, t, 0}, Vec3{-1, -t, 0}, Vec3{1, -t, 0},
                   Vec3{0, -1, t}, Vec3{0, 1, t}, Vec3{0, -1, -t}, Vec3{0, 1, -t},
                   Vec3{t, 0, -1}, Vec3{t, 0, 1}, Vec3{-t, 0, -1}, Vec3{-t, 0, 1}}) {
        sphere.points.push_back(iron::normalize(p));
    }
    sphere.triangles = {0, 11, 5,  0, 5, 1,  0, 1, 7,  0, 7, 10, 0, 10, 11,
                        1, 5, 9,   5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                        3, 9, 4,   3, 4, 2,  3, 2, 6,  3, 6, 8,  3, 8, 9,
                        4, 9, 5,   2, 4, 11, 6, 2, 10, 8, 6, 7,  9, 8, 1};

    for (int level = 0; level < subdivisions; ++level) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> midpoints;
        const auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::minmax(a, b);
            const auto found = midpoints.find({key.first, key.second});
            if (found != midpoints.end()) return found->second;
            const auto index = static_cast<std::uint32_t>(sphere.points.size());
            sphere.points.push_back(iron::normalize(sphere.points[a] + sphere.points[b]));
            midpoints[{key.first, key.second}] = index;
            return index;
        };

        std::vector<std::uint32_t> finer;
        finer.reserve(sphere.triangles.size() * 4);
        for (std::size_t i = 0; i + 2 < sphere.triangles.size(); i += 3) {
            const std::uint32_t a = sphere.triangles[i];
            const std::uint32_t b = sphere.triangles[i + 1];
            const std::uint32_t c = sphere.triangles[i + 2];
            const std::uint32_t ab = midpoint(a, b);
            const std::uint32_t bc = midpoint(b, c);
            const std::uint32_t ca = midpoint(c, a);
            finer.insert(finer.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        sphere.triangles = std::move(finer);
    }
    return sphere;
}

void smoothNormals(MeshData& mesh) {
    for (Vertex& v : mesh.vertices) v.normal = {0, 0, 0};
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        Vertex& a = mesh.vertices[mesh.indices[i]];
        Vertex& b = mesh.vertices[mesh.indices[i + 1]];
        Vertex& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 n = iron::cross(b.position - a.position, c.position - a.position);
        a.normal = a.normal + n;
        b.normal = b.normal + n;
        c.normal = c.normal + n;
    }
    for (Vertex& v : mesh.vertices) {
        v.normal = iron::dot(v.normal, v.normal) > 1e-14f ? iron::normalize(v.normal) : Vec3{0, 1, 0};
    }
}

// A tangent perpendicular to the normal, and UVs from the dominant axes.
void finishSurface(MeshData& mesh, float uvScale) {
    for (Vertex& v : mesh.vertices) {
        const Vec3 helper = std::abs(v.normal.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        v.tangent = iron::normalize(iron::cross(helper, v.normal));
        v.uv = {(v.position.x + v.position.z * 0.7f) * uvScale,
                (v.position.y + v.position.z * 0.7f) * uvScale};
    }
}

Vec3 rotateAbout(Vec3 v, Vec3 axis, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return v * c + iron::cross(axis, v) * s + axis * (iron::dot(axis, v) * (1.0f - c));
}

// Outer surface of the shell: the same for every plate, so that plates meet.
float shellRadius(Vec3 dir, float radius, std::uint32_t seed) {
    return radius * (1.0f + 0.05f * (fbm(dir * 2.2f, seed, 5) - 0.5f));
}

}  // namespace

std::vector<RockInstance> makeRockField(std::uint32_t seed, int count) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    std::vector<RockInstance> rocks;
    rocks.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        RockInstance rock;
        const float angle = kTau * unit(rng);
        const float radius = 8.0f + 20.0f * std::pow(unit(rng), 0.8f);
        const float height = (unit(rng) - 0.5f) * 12.0f;
        rock.home = {std::cos(angle) * radius, height, std::sin(angle) * radius};

        const float z = 2.0f * unit(rng) - 1.0f;
        const float a = kTau * unit(rng);
        const float ring = std::sqrt(std::max(1.0f - z * z, 0.0f));
        rock.axis = {ring * std::cos(a), z, ring * std::sin(a)};
        rock.spin = (0.05f + 0.25f * unit(rng)) * (unit(rng) < 0.5f ? -1.0f : 1.0f);

        const float sizeRoll = unit(rng);
        rock.scale = 0.22f + 1.2f * sizeRoll * sizeRoll * sizeRoll;
        rock.shadowRadius = rock.scale * 1.05f;
        rock.variant = static_cast<int>(unit(rng) * kRockVariants) % kRockVariants;
        rock.ringRadius = 7.0f + 8.0f * unit(rng);
        rock.ringAngle = kTau * unit(rng);
        rocks.push_back(rock);
    }
    return rocks;
}

std::vector<MeshData> makeShellPlates(int plateCount, float radius, std::uint32_t seed,
                                      std::vector<RockInstance>& plates) {
    constexpr int kSubdivisions = 6;
    constexpr float kThickness = 0.42f;
    constexpr float kGap = 0.988f;  // Pulling each plate in opens the glowing cracks.

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    // Evenly spread seeds, so that plates come out a similar size.
    std::vector<Vec3> seeds;
    seeds.reserve(static_cast<std::size_t>(plateCount));
    const float golden = std::numbers::pi_v<float> * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < plateCount; ++i) {
        const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(plateCount);
        const float ring = std::sqrt(std::max(1.0f - y * y, 0.0f));
        const float angle = golden * static_cast<float>(i) + 0.3f * unit(rng);
        seeds.push_back(iron::normalize(Vec3{std::cos(angle) * ring, y + 0.1f * (unit(rng) - 0.5f),
                                             std::sin(angle) * ring}));
    }

    const Icosphere sphere = makeIcosphere(kSubdivisions);

    // Warping the lookup makes plate edges jagged, not straight Voronoi lines.
    const auto nearestSeed = [&](Vec3 dir) {
        const Vec3 warp{fbm(dir * 3.1f, seed + 7u, 3) - 0.25f, fbm(dir * 3.1f, seed + 17u, 3) - 0.25f,
                        fbm(dir * 3.1f, seed + 29u, 3) - 0.25f};
        const Vec3 warped = iron::normalize(dir + warp * 0.55f);
        int best = 0;
        float bestDot = -2.0f;
        for (int i = 0; i < plateCount; ++i) {
            const float d = iron::dot(warped, seeds[static_cast<std::size_t>(i)]);
            if (d > bestDot) {
                bestDot = d;
                best = i;
            }
        }
        return best;
    };

    std::vector<int> owner(sphere.points.size());
    for (std::size_t i = 0; i < sphere.points.size(); ++i) owner[i] = nearestSeed(sphere.points[i]);

    std::vector<std::vector<std::uint32_t>> plateTriangles(static_cast<std::size_t>(plateCount));
    for (std::size_t i = 0; i + 2 < sphere.triangles.size(); i += 3) {
        const int plate = owner[sphere.triangles[i]];
        auto& tris = plateTriangles[static_cast<std::size_t>(plate)];
        tris.insert(tris.end(), {sphere.triangles[i], sphere.triangles[i + 1], sphere.triangles[i + 2]});
    }

    std::vector<MeshData> meshes;
    meshes.reserve(static_cast<std::size_t>(plateCount));
    plates.clear();

    for (int p = 0; p < plateCount; ++p) {
        const auto& tris = plateTriangles[static_cast<std::size_t>(p)];

        std::map<std::uint32_t, std::uint32_t> remap;
        std::vector<Vec3> outer;
        for (std::uint32_t index : tris) {
            if (remap.emplace(index, static_cast<std::uint32_t>(outer.size())).second) {
                const Vec3 dir = sphere.points[index];
                outer.push_back(dir * shellRadius(dir, radius, seed));
            }
        }

        // Triangle edges make a plate's rim zigzag. Relaxing each rim vertex
        // toward its neighbours turns that into a clean fracture line.
        {
            std::map<std::pair<std::uint32_t, std::uint32_t>, int> rimEdges;
            for (std::size_t i = 0; i + 2 < tris.size(); i += 3) {
                const std::uint32_t a = remap[tris[i]];
                const std::uint32_t b = remap[tris[i + 1]];
                const std::uint32_t c = remap[tris[i + 2]];
                for (auto [from, to] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
                    rimEdges[{from, to}] += 1;
                }
            }
            std::vector<std::vector<std::uint32_t>> rimNeighbours(outer.size());
            for (const auto& [edge, used] : rimEdges) {
                if (rimEdges.count({edge.second, edge.first}) != 0) continue;
                rimNeighbours[edge.first].push_back(edge.second);
                rimNeighbours[edge.second].push_back(edge.first);
            }
            for (int pass = 0; pass < 12; ++pass) {
                std::vector<Vec3> relaxed = outer;
                for (std::size_t i = 0; i < outer.size(); ++i) {
                    if (rimNeighbours[i].size() != 2) continue;
                    const Vec3 mid = (outer[rimNeighbours[i][0]] + outer[rimNeighbours[i][1]]) * 0.5f;
                    const Vec3 dir = iron::normalize(outer[i] + (mid - outer[i]) * 0.6f);
                    relaxed[i] = dir * shellRadius(dir, radius, seed);
                }
                outer = std::move(relaxed);
            }
        }

        Vec3 centroid{0, 0, 0};
        for (Vec3 v : outer) centroid = centroid + v;
        centroid = centroid * (1.0f / static_cast<float>(std::max<std::size_t>(outer.size(), 1)));

        MeshData mesh;
        const auto outerCount = static_cast<std::uint32_t>(outer.size());
        mesh.vertices.resize(outerCount * 2);
        for (std::uint32_t i = 0; i < outerCount; ++i) {
            const Vec3 local = (outer[i] - centroid) * kGap;
            const Vec3 inward = iron::normalize(outer[i]) * -kThickness;
            mesh.vertices[i].position = local;
            mesh.vertices[outerCount + i].position = local + inward;
        }

        std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
        for (std::size_t i = 0; i + 2 < tris.size(); i += 3) {
            const std::uint32_t a = remap[tris[i]];
            const std::uint32_t b = remap[tris[i + 1]];
            const std::uint32_t c = remap[tris[i + 2]];
            mesh.indices.insert(mesh.indices.end(), {a, b, c});
            mesh.indices.insert(mesh.indices.end(),
                                {outerCount + a, outerCount + c, outerCount + b});
            for (auto [from, to] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
                edges[{from, to}] += 1;
            }
        }

        // An edge with no opposite twin lies on the plate's rim, so it gets a wall.
        for (const auto& [edge, used] : edges) {
            if (edges.count({edge.second, edge.first}) != 0) continue;
            const std::uint32_t a = edge.first;
            const std::uint32_t b = edge.second;
            mesh.indices.insert(mesh.indices.end(),
                                {a, outerCount + a, b, b, outerCount + a, outerCount + b});
        }

        smoothNormals(mesh);
        finishSurface(mesh, 0.5f);
        meshes.push_back(std::move(mesh));

        RockInstance plate;
        plate.home = centroid;
        float extent = 0.0f;
        for (const Vertex& v : meshes.back().vertices) {
            extent = std::max(extent, iron::length(v.position));
        }
        plate.shadowRadius = extent * 0.72f;
        const Vec3 tumble{unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f};
        plate.axis = iron::normalize(tumble + Vec3{0.01f, 0.02f, 0.03f});
        plate.spin = 0.6f + 1.2f * unit(rng);
        plate.ringRadius = 6.0f + 6.0f * unit(rng);
        plate.ringAngle = kTau * unit(rng);
        plate.plate = true;
        plates.push_back(plate);
    }
    return meshes;
}

MeshData makeRock(std::uint32_t seed) {
    constexpr int kSubdivisions = 4;

    const Icosphere sphere = makeIcosphere(kSubdivisions);
    MeshData mesh;
    mesh.vertices.resize(sphere.points.size());
    mesh.indices = sphere.triangles;

    for (std::size_t i = 0; i < sphere.points.size(); ++i) {
        const Vec3 dir = sphere.points[i];
        const float lumps = fbm(dir * 1.3f + Vec3{3.1f, 1.7f, 0.0f}, seed, 3);
        const float cracks = std::abs(fbm(dir * 3.4f, seed + 53u, 4) - 0.5f);
        const float radius = 0.55f + 0.85f * lumps + 0.35f * cracks;
        mesh.vertices[i].position = {dir.x * radius, dir.y * radius * 0.82f, dir.z * radius};
    }
    smoothNormals(mesh);
    finishSurface(mesh, 1.3f);
    return mesh;
}

Vec3 rockPosition(const RockInstance& rock, float t) {
    constexpr float kDriftSpeed = 0.012f;
    constexpr float kRingSpeed = 0.1f;

    const Beat beat = evaluateBeat(t);

    Vec3 position = rock.home;
    if (!rock.plate) position = rotateAbout(rock.home, Vec3{0, 1, 0}, t * kDriftSpeed);

    // The blast throws each piece outward from the moment the front reaches it.
    const float dist = std::max(iron::length(position), 0.01f);
    const Vec3 outward = position * (1.0f / dist);
    if (beat.shockRadius > dist) {
        const float hitTime = shockArrival(dist);
        const float speed = rock.plate ? 18.0f : 26.0f / (1.0f + rock.scale * rock.scale * 2.0f);
        position = position + outward * (speed * (1.0f - std::exp(-(t - hitTime) * 0.55f)));
    }

    if (beat.rebirth > 0.0f) {
        // Falling back in, the pieces settle into tilted rings around the star.
        const float angle = rock.ringAngle + t * kRingSpeed * (16.0f / (rock.ringRadius + 4.0f));
        const Vec3 flat{std::cos(angle) * rock.ringRadius, 0.0f, std::sin(angle) * rock.ringRadius};
        const Vec3 ring = rotateAbout(flat, Vec3{1, 0, 0}, kRingTilt);
        position = position + (ring - position) * beat.rebirth;
    }
    return position;
}

float rockAngle(const RockInstance& rock, float t) {
    if (rock.plate) {
        return t > kIgnitionTime ? (t - kIgnitionTime) * rock.spin : 0.0f;
    }
    const float kick = t > kIgnitionTime ? (1.0f - std::exp(-(t - kIgnitionTime))) * 3.0f : 0.0f;
    return t * rock.spin + kick * rock.spin;
}

float lightVisibility(Vec3 eye, Vec3 light, float lightRadius, std::span<const iron::Vec4> spheres) {
    constexpr int kRays = 13;
    const float golden = std::numbers::pi_v<float> * (3.0f - std::sqrt(5.0f));

    const Vec3 forward = iron::normalize(light - eye);
    const Vec3 right = iron::normalize(iron::cross(forward, Vec3{0, 1, 0}));
    const Vec3 up = iron::cross(right, forward);

    int visible = 0;
    for (int i = 0; i < kRays; ++i) {
        // Rays spread evenly across the disc of the light.
        const float r = lightRadius * std::sqrt((static_cast<float>(i) + 0.5f) / kRays);
        const float angle = golden * static_cast<float>(i);
        const Vec3 target = light + right * (std::cos(angle) * r) + up * (std::sin(angle) * r);
        const Vec3 toTarget = target - eye;
        const float length = iron::length(toTarget);
        const Vec3 dir = toTarget * (1.0f / length);

        bool blocked = false;
        for (const iron::Vec4& s : spheres) {
            const Vec3 toCenter = Vec3{s.x, s.y, s.z} - eye;
            const float along = iron::dot(toCenter, dir);
            if (along <= 0.0f || along >= length) continue;
            const float offAxis = iron::dot(toCenter, toCenter) - along * along;
            if (offAxis < s.w * s.w) {
                blocked = true;
                break;
            }
        }
        if (!blocked) ++visible;
    }
    return static_cast<float>(visible) / kRays;
}

std::vector<float> makeTitleParticles(const iron::FontAtlas& font, const std::string& text,
                                      int count, float width, float baselineY, float tracking,
                                      float aspect, std::uint32_t seed) {
    struct Box {
        iron::GlyphQuad quad;
        float area;
    };

    const float gap = font.pixelHeight() * tracking;
    float penX = 0.0f;
    float penY = 0.0f;
    std::vector<Box> boxes;
    float totalArea = 0.0f;
    for (char c : text) {
        const iron::GlyphQuad q = font.quadFor(c, penX, penY);
        penX += gap;
        const float area = (q.max.x - q.min.x) * (q.max.y - q.min.y);
        if (area > 0.0f) {
            boxes.push_back({q, area});
            totalArea += area;
        }
    }
    const float textWidth = std::max(penX - gap, 1.0f);
    const float scale = width / textWidth;

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const auto& pixels = font.pixels();

    std::vector<float> data(static_cast<std::size_t>(count) * 16, 0.0f);
    int placed = 0;
    int attempts = 0;
    const int maxAttempts = count * 400;
    while (placed < count && attempts < maxAttempts && !boxes.empty()) {
        ++attempts;

        float pick = unit(rng) * totalArea;
        const Box* box = &boxes.back();
        for (const Box& candidate : boxes) {
            pick -= candidate.area;
            if (pick <= 0.0f) {
                box = &candidate;
                break;
            }
        }

        const float u = unit(rng);
        const float v = unit(rng);
        const iron::GlyphQuad& q = box->quad;
        const float atlasU = q.uvMin.x + (q.uvMax.x - q.uvMin.x) * u;
        const float atlasV = q.uvMin.y + (q.uvMax.y - q.uvMin.y) * v;
        const int ax = std::clamp(static_cast<int>(atlasU * font.width()), 0, font.width() - 1);
        const int ay = std::clamp(static_cast<int>(atlasV * font.height()), 0, font.height() - 1);
        const std::size_t alpha = (static_cast<std::size_t>(ay) * font.width() + ax) * 4 + 3;
        if (pixels[alpha] < 128) continue;

        const float px = q.min.x + (q.max.x - q.min.x) * u;
        const float py = q.min.y + (q.max.y - q.min.y) * v;

        const std::size_t i = static_cast<std::size_t>(placed) * 16;
        data[i + 7]  = 1.0f;  // Lifetime, so that a particle counts as spawned.
        data[i + 8]  = (px - textWidth * 0.5f) * scale;
        data[i + 9]  = baselineY - py * scale * aspect;  // Clip Y points up, text Y down.
        data[i + 10] = px / textWidth;
        data[i + 11] = unit(rng);
        ++placed;
    }

    // A font without enough ink is a caller bug, but the extras must stay harmless.
    data.resize(static_cast<std::size_t>(count) * 16, 0.0f);
    return data;
}

std::vector<std::uint8_t> makeRockNormalMap(int size) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size) * size * 4);

    // Sampling a circle in 3D noise for each axis gives a seamless tile.
    const auto height = [](float u, float v) {
        const float a = kTau * u;
        const float b = kTau * v;
        const Vec3 p{std::cos(a) * 1.6f, std::sin(a) * 1.6f, std::cos(b) * 1.6f};
        const Vec3 q{std::sin(b) * 1.6f, 0.0f, 0.0f};
        return fbm(p + q * 1.0f + Vec3{4, 4, 4}, 91u, 6);
    };

    constexpr float kStep = 1.0f / 512.0f;
    constexpr float kStrength = 1.4f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(size);
            const float v = static_cast<float>(y) / static_cast<float>(size);
            const float du = (height(u + kStep, v) - height(u - kStep, v)) * kStrength / (2.0f * kStep) * 0.02f;
            const float dv = (height(u, v + kStep) - height(u, v - kStep)) * kStrength / (2.0f * kStep) * 0.02f;
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

}  // namespace ironcore
