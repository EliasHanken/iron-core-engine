#include "physics/SkinnedRagdoll.h"

#include "math/Transform.h"
#include "test_framework.h"

#include <cmath>
#include <vector>

using namespace iron;

namespace {

// A stick figure standing on the origin facing -z: hips at 1 m, chest above,
// head on top (its tip a bone at 1.85 m), and a "rifle" bone held out in
// front of the chest carrying a 0.8 m long run of vertices along -z.
struct Figure {
    Skeleton          skeleton;
    SkinnedMeshData   skin;
    std::vector<Mat4> globals;
};

Figure figure() {
    Figure f;
    const auto bone = [&](const char* name, int parent, Vec3 at) {
        Bone b;
        b.name = name;
        b.parentIndex = parent;
        b.inverseBindMatrix = translation(at * -1.0f);
        f.skeleton.bones.push_back(b);
        f.globals.push_back(translation(at));
    };
    bone("hips", -1, {0.0f, 1.0f, 0.0f});
    bone("chest", 0, {0.0f, 1.3f, 0.0f});
    bone("head", 1, {0.0f, 1.6f, 0.0f});
    bone("crown", 2, {0.0f, 1.85f, 0.0f});
    bone("rifle", 1, {0.2f, 1.4f, -0.3f});
    for (float z = 0.0f; z >= -0.8f; z -= 0.1f) {
        SkinnedVertex v{};
        v.position = {0.2f, 1.4f, -0.3f + z};
        v.joints[0] = 4;
        v.weights[0] = 1.0f;
        f.skin.vertices.push_back(v);
    }
    for (std::size_t i = 0; i < f.skeleton.bones.size(); ++i) {
        f.skeleton.bones[i].localBindTransform =
            f.skeleton.bones[i].parentIndex >= 0
                ? inverse(f.globals[static_cast<std::size_t>(f.skeleton.bones[i].parentIndex)]) * f.globals[i]
                : f.globals[i];
    }
    return f;
}

std::vector<RagdollPartSpec> parts() {
    std::vector<RagdollPartSpec> p(4);
    p[0] = {"hips", "chest", 0.15f, 12.0f, "", RagdollJoint::None};
    p[1] = {"chest", "head", 0.16f, 18.0f, "hips", RagdollJoint::SwingTwist, 0.5f, 0.3f};
    p[2] = {"head", "crown", 0.11f, 5.0f, "chest", RagdollJoint::SwingTwist, 0.6f, 0.5f};
    p[2].damage = 4.0f;
    p[3] = {"rifle", "", 0.0f, 3.5f, "", RagdollJoint::None};
    return p;
}

Vec3 at(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }

}  // namespace

static void rays_find_the_part_they_hit() {
    Figure f = figure();
    SkinnedRagdoll r;
    const auto specs = parts();
    const SkinnedMeshData* skins[] = {&f.skin};
    CHECK(r.setup(f.skeleton, specs, skins));
    // Standing 5 m along +x: shoot it in the head, the chest, over it.
    const Mat4 model = translation({5.0f, 0.0f, 0.0f});
    const Vec3 dir{-1.0f, 0.0f, 0.0f};
    const RagdollHit head = r.raycast(f.globals, model, {10.0f, 1.72f, 0.0f}, dir, 100.0f);
    CHECK(head.part == 2);
    CHECK(std::abs(head.distance - (10.0f - 5.0f - 0.11f)) < 0.02f);
    CHECK(r.raycast(f.globals, model, {10.0f, 1.45f, 0.0f}, dir, 100.0f).part == 1);
    CHECK(r.raycast(f.globals, model, {10.0f, 1.1f, 0.0f}, dir, 100.0f).part == 0);
    CHECK(r.raycast(f.globals, model, {10.0f, 2.2f, 0.0f}, dir, 100.0f).part == -1);
    CHECK(r.raycast(f.globals, model, {10.0f, 1.72f, 0.0f}, dir, 3.0f).part == -1);  // Out of reach.
    // The rifle's box runs from z -0.3 to -1.1 in front of the chest.
    CHECK(r.raycast(f.globals, model, {10.0f, 1.4f, -0.9f}, dir, 100.0f).part == 3);
    CHECK(r.raycast(f.globals, model, {10.0f, 1.4f, -1.3f}, dir, 100.0f).part == -1);
}

static void it_falls_jointed_and_drops_the_rifle() {
    Figure f = figure();
    SkinnedRagdoll r;
    const auto specs = parts();
    const SkinnedMeshData* skins[] = {&f.skin};
    CHECK(r.setup(f.skeleton, specs, skins));
    PhysicsWorld world;
    CHECK(world.init());
    world.createStaticBox({0.0f, -0.5f, 0.0f}, {20.0f, 0.5f, 20.0f});
    const Mat4 model = Mat4::identity();
    r.spawn(world, f.globals, model, {0.0f, 0.0f, 0.0f}, 7u);
    CHECK(r.spawned());

    // Spawned, it holds the pose it died in.
    std::vector<Mat4> posed(f.globals.size());
    r.pose(posed);
    for (std::size_t i = 0; i < posed.size(); ++i) CHECK(length(at(posed[i]) - at(f.globals[i])) < 1e-3f);

    // A shove to the chest, and it goes down.
    r.push(1, {0.0f, 0.0f, 60.0f}, {0.0f, 1.45f, 0.0f});
    for (int i = 0; i < 240; ++i) world.step(1.0f / 60.0f);
    r.pose(posed);
    const Vec3 hips = at(posed[0]), chest = at(posed[1]), head = at(posed[2]), rifle = at(posed[4]);
    CHECK(head.y < 0.6f);                                        // Down on the floor.
    CHECK(hips.y > -0.05f && head.y > -0.05f);                   // Not through it.
    CHECK(std::abs(length(chest - hips) - 0.3f) < 0.05f);        // Still jointed.
    CHECK(std::abs(length(head - chest) - 0.3f) < 0.05f);
    CHECK(rifle.y < 0.4f);                                       // The rifle fell too...
    for (const Mat4& m : posed) CHECK(std::isfinite(m.at(0, 3)) && std::isfinite(m.at(1, 3)));
    r.despawn();
    CHECK(!r.spawned());
}

int main() {
    rays_find_the_part_they_hit();
    it_falls_jointed_and_drops_the_rifle();
    return iron_test_result();
}
