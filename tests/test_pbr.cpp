// CPU port of the PBR BRDF helpers (kept in lockstep with the GLSL in
// StandardLitShader.h). Verifies F0, Fresnel, GGX-D, and Smith-G properties.
#include "render/Pbr.h"
#include "scene/Mesh.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using iron::Vec3;

static bool approx(float a, float b, float eps = 1e-4f) {
    float d = a - b; return (d < 0 ? -d : d) <= eps;
}

int main() {
    // Dielectric F0 ~= 0.04 (per channel).
    {
        Vec3 f0 = iron::f0For(Vec3{0.8f, 0.1f, 0.1f}, 0.0f);
        assert(approx(f0.x, 0.04f) && approx(f0.y, 0.04f) && approx(f0.z, 0.04f));
    }
    // Metallic F0 == albedo.
    {
        Vec3 albedo{0.8f, 0.1f, 0.1f};
        Vec3 f0 = iron::f0For(albedo, 1.0f);
        assert(approx(f0.x, albedo.x) && approx(f0.y, albedo.y) && approx(f0.z, albedo.z));
    }
    // Fresnel: at normal incidence (cos=1) returns F0; at grazing (cos=0) -> ~1.
    {
        Vec3 f0{0.04f, 0.04f, 0.04f};
        Vec3 at0 = iron::fresnelSchlick(1.0f, f0);
        Vec3 at90 = iron::fresnelSchlick(0.0f, f0);
        assert(approx(at0.x, 0.04f));
        assert(at90.x > 0.99f);
    }
    // GGX D: at fixed NdotH=1, lower roughness => higher (sharper) peak.
    {
        float dRough = iron::distributionGGX(1.0f, 0.8f);
        float dSharp = iron::distributionGGX(1.0f, 0.1f);
        assert(dSharp > dRough);
    }
    // Smith G in [0,1] and increases with NdotV/NdotL.
    {
        float g = iron::geometrySmith(0.9f, 0.9f, 0.5f);
        assert(g >= 0.0f && g <= 1.0f);
        float gLow = iron::geometrySmith(0.2f, 0.2f, 0.5f);
        assert(g > gLow);
    }
    // makeUVSphere: vertex/index counts, unit normals, points on the sphere.
    {
        iron::MeshData m = iron::makeUVSphere(2.0f, 16);  // radius 2, 16 segments
        assert(!m.vertices.empty() && !m.indices.empty());
        for (const auto& v : m.vertices) {
            float len = std::sqrt(v.position.x*v.position.x + v.position.y*v.position.y + v.position.z*v.position.z);
            assert(approx(len, 2.0f, 1e-3f));           // on the sphere of radius 2
            float nl = std::sqrt(v.normal.x*v.normal.x + v.normal.y*v.normal.y + v.normal.z*v.normal.z);
            assert(approx(nl, 1.0f, 1e-3f));            // unit normal
        }
        assert(m.indices.size() % 3 == 0);              // triangle list
        // Winding: every triangle must face OUTWARD (front-face CCW under the
        // CULL_BACK pipeline). For a sphere centered at origin, the face normal
        // cross(e1,e2) must point the same way as the triangle centroid.
        for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const auto& a = m.vertices[m.indices[t]].position;
            const auto& b = m.vertices[m.indices[t + 1]].position;
            const auto& c = m.vertices[m.indices[t + 2]].position;
            Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
            Vec3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
            Vec3 nrm{e1.y * e2.z - e1.z * e2.y,
                     e1.z * e2.x - e1.x * e2.z,
                     e1.x * e2.y - e1.y * e2.x};
            float area2 = std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
            if (area2 < 1e-5f) continue;                // skip degenerate pole tris
            Vec3 centroid{(a.x + b.x + c.x) / 3.0f,
                          (a.y + b.y + c.y) / 3.0f,
                          (a.z + b.z + c.z) / 3.0f};
            float facing = nrm.x * centroid.x + nrm.y * centroid.y + nrm.z * centroid.z;
            assert(facing > 0.0f);                      // outward-facing (CCW from outside)
        }
    }
    std::puts("test_pbr: all passed");
    return 0;
}
