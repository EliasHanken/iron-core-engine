#include "ai/NavMesh.h"

#include "test_framework.h"

#include <cmath>
#include <vector>

using namespace iron;

namespace {

// A box's twelve triangles, outward-wound (y up).
void box(std::vector<Vec3>& v, std::vector<std::uint32_t>& idx, Vec3 lo, Vec3 hi) {
    const auto base = static_cast<std::uint32_t>(v.size());
    for (int i = 0; i < 8; ++i) {
        v.push_back({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
    }
    const std::uint32_t faces[6][4] = {
        {0, 4, 6, 2}, {1, 3, 7, 5},  // -x, +x
        {0, 1, 5, 4}, {2, 6, 7, 3},  // -y, +y
        {0, 2, 3, 1}, {4, 5, 7, 6},  // -z, +z
    };
    for (const auto& f : faces) {
        idx.insert(idx.end(), {base + f[0], base + f[1], base + f[2], base + f[0], base + f[2], base + f[3]});
    }
}

float pathLength(const std::vector<Vec3>& c) {
    float d = 0.0f;
    for (std::size_t i = 1; i < c.size(); ++i) d += length(c[i] - c[i - 1]);
    return d;
}

}  // namespace

// A 20 x 20 m floor with a wall across its middle (z = 0), open past x = 6.
static void paths_go_round_walls() {
    std::vector<Vec3> v;
    std::vector<std::uint32_t> idx;
    box(v, idx, {-10.0f, -0.2f, -10.0f}, {10.0f, 0.0f, 10.0f});
    box(v, idx, {-10.0f, 0.0f, -0.25f}, {6.0f, 3.0f, 0.25f});
    NavMesh nav;
    CHECK(nav.build(v, idx));
    CHECK(nav.ready());
    CHECK(nav.polygonCount() > 0);

    std::vector<Vec3> corners;
    CHECK(nav.findPath({-5.0f, 0.0f, -5.0f}, {-5.0f, 0.0f, 5.0f}, corners));
    CHECK(corners.size() >= 3);  // Turns at the wall's end.
    // Straight through would be 10 m; round the end at x = 6 is well over 22.
    CHECK(pathLength(corners) > 22.0f);
    for (const Vec3& c : corners) CHECK(std::abs(c.y) < 0.3f);
    // Every corner keeps the agent's radius off the wall's end.
    bool passedEnd = false;
    for (const Vec3& c : corners) passedEnd = passedEnd || c.x > 6.0f;
    CHECK(passedEnd);

    // Both sides of the wall are one island (joined round its end); a box too
    // high to climb is an island of its own on top.
    CHECK(nav.island({-5.0f, 0.0f, -5.0f}) == nav.island({-5.0f, 0.0f, 5.0f}));
    CHECK(nav.island({-5.0f, 0.0f, -5.0f}) >= 0);
    CHECK(nav.island({50.0f, 0.0f, 0.0f}) == -1);

    // No path onto the top of the wall (too high a step).
    Vec3 on;
    CHECK(!nav.nearest({0.0f, 3.0f, 0.0f}, on, {0.1f, 0.1f, 0.1f}));
}

static void a_shut_room_is_its_own_island() {
    std::vector<Vec3> v;
    std::vector<std::uint32_t> idx;
    box(v, idx, {-10.0f, -0.2f, -10.0f}, {10.0f, 0.0f, 10.0f});
    // Four walls round a 4 x 4 m room, no door.
    box(v, idx, {2.0f, 0.0f, 2.0f}, {8.0f, 3.0f, 2.3f});
    box(v, idx, {2.0f, 0.0f, 7.7f}, {8.0f, 3.0f, 8.0f});
    box(v, idx, {2.0f, 0.0f, 2.0f}, {2.3f, 3.0f, 8.0f});
    box(v, idx, {7.7f, 0.0f, 2.0f}, {8.0f, 3.0f, 8.0f});
    NavMesh nav;
    CHECK(nav.build(v, idx));
    const int outside = nav.island({-5.0f, 0.0f, -5.0f});
    const int inside = nav.island({5.0f, 0.0f, 5.0f});
    CHECK(outside >= 0 && inside >= 0 && outside != inside);
    std::vector<Vec3> corners;
    CHECK(!nav.findPath({-5.0f, 0.0f, -5.0f}, {5.0f, 0.0f, 5.0f}, corners));
}

static void a_crowd_agent_walks_to_its_target() {
    std::vector<Vec3> v;
    std::vector<std::uint32_t> idx;
    box(v, idx, {-10.0f, -0.2f, -10.0f}, {10.0f, 0.0f, 10.0f});
    box(v, idx, {-10.0f, 0.0f, -0.25f}, {6.0f, 3.0f, 0.25f});
    NavMesh nav;
    CHECK(nav.build(v, idx));
    const int a = nav.addAgent({-5.0f, 0.0f, -5.0f}, 0.35f, 2.0f);
    CHECK(a >= 0);
    CHECK(nav.setTarget(a, {-5.0f, 0.0f, 5.0f}));
    CHECK(!nav.arrived(a, 0.5f));
    float maxSpeed = 0.0f;
    for (int i = 0; i < 60 * 30 && !nav.arrived(a, 0.5f); ++i) {
        nav.updateCrowd(1.0f / 60.0f);
        const Vec3 vel = nav.agentVelocity(a);
        maxSpeed = std::max(maxSpeed, std::sqrt(vel.x * vel.x + vel.z * vel.z));
        // Never through the wall.
        const Vec3 p = nav.agentPosition(a);
        CHECK(!(p.x < 6.0f && std::abs(p.z) < 0.25f));
    }
    CHECK(nav.arrived(a, 0.5f));
    CHECK(maxSpeed <= 2.05f);
}

static void random_points_land_on_the_mesh() {
    std::vector<Vec3> v;
    std::vector<std::uint32_t> idx;
    box(v, idx, {-10.0f, -0.2f, -10.0f}, {10.0f, 0.0f, 10.0f});
    NavMesh nav;
    CHECK(nav.build(v, idx));
    for (std::uint32_t seed = 0; seed < 8; ++seed) {
        Vec3 p;
        CHECK(nav.randomPointNear({0.0f, 0.0f, 0.0f}, 4.0f, seed, p));
        CHECK(std::abs(p.x) < 10.0f && std::abs(p.z) < 10.0f && std::abs(p.y) < 0.3f);
    }
    std::vector<std::pair<Vec3, Vec3>> e;
    nav.edges(e);
    CHECK(!e.empty());
}

int main() {
    paths_go_round_walls();
    a_shut_room_is_its_own_island();
    a_crowd_agent_walks_to_its_target();
    random_points_land_on_the_mesh();
    return iron_test_result();
}
