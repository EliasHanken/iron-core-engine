#include "math/Quaternion.h"
#include "physics/Ballistics.h"
#include "physics/PhysicsWorld.h"

#include "test_framework.h"

#include <cmath>

using namespace iron;

using Kind = BulletImpact::Kind;

static PhysicalMaterial wood() {
    PhysicalMaterial m;
    m.name = "wood";
    m.resistance = 2.0f;  // Half a metre of it.
    return m;
}
static PhysicalMaterial concrete() {
    PhysicalMaterial m;
    m.name = "concrete";
    m.resistance = 12.0f;  // About 8 cm.
    return m;
}
static PhysicalMaterial steel() {
    PhysicalMaterial m;
    m.name = "steel";
    m.resistance = 150.0f;
    m.ricochetAngle = 0.35f;  // About 20 degrees.
    return m;
}

static void through_wood_into_concrete() {
    PhysicsWorld world;
    CHECK(world.init());
    PhysicalMaterials materials;
    const auto woodId = materials.add(wood());
    const auto concreteId = materials.add(concrete());
    // A 10 cm plank at z = -5 and a 40 cm wall at z = -10, across the path.
    const BodyId plank = world.createStaticBox({0, 0, -5}, {2, 2, 0.05f});
    const BodyId wall = world.createStaticBox({0, 0, -10}, {2, 2, 0.2f});
    materials.assign(plank, woodId);
    materials.assign(wall, concreteId);
    world.step(1.0f / 60.0f);

    const auto impacts = traceBullet(world, materials, {0, 0, 0}, {0, 0, -1});
    CHECK(impacts.size() == 3);
    if (impacts.size() == 3) {
        CHECK(impacts[0].kind == Kind::Entry && impacts[0].body == plank);
        CHECK_NEAR(impacts[0].point.z, -4.95f);
        CHECK(impacts[1].kind == Kind::Exit && impacts[1].body == plank);
        CHECK_NEAR(impacts[1].point.z, -5.05f);
        CHECK(impacts[1].normal.z < -0.9f);  // Out of the far side.
        CHECK_NEAR(impacts[1].energy, 1.0f - 0.1f * 2.0f);
        CHECK(impacts[2].kind == Kind::Stop && impacts[2].body == wall);
        CHECK(impacts[2].normal.z > 0.9f);  // Facing the round.
    }
    world.shutdown();
}

static void glancing_off_steel() {
    PhysicsWorld world;
    CHECK(world.init());
    PhysicalMaterials materials;
    const auto steelId = materials.add(steel());
    // A floor plate, and a round coming in at 10 degrees.
    const BodyId plate = world.createStaticBox({0, -1, -10}, {5, 0.05f, 15});
    materials.assign(plate, steelId);
    world.step(1.0f / 60.0f);
    const float a = 0.1745f;
    const auto impacts = traceBullet(world, materials, {0, 0, 0}, {0, -std::sin(a), -std::cos(a)});
    CHECK(!impacts.empty());
    if (!impacts.empty()) {
        CHECK(impacts[0].kind == Kind::Ricochet);
        CHECK(impacts[0].direction.y > 0.0f);  // Leaving upwards.
    }
    // Straight down, it doesn't glance: it stops.
    const auto square = traceBullet(world, materials, {0, 0, -10}, {0, -1, 0});
    CHECK(square.size() == 1 && square[0].kind == Kind::Stop);
    world.shutdown();
}

static void a_hollow_crate_costs_two_walls() {
    PhysicsWorld world;
    CHECK(world.init());
    PhysicalMaterials materials;
    PhysicalMaterial crate = wood();
    crate.shell = 0.02f;
    const auto crateId = materials.add(crate);
    // A metre deep: solid wood would stop the round, two 2 cm walls don't.
    const BodyId box = world.createStaticBox({0, 0, -5}, {0.5f, 0.5f, 0.5f});
    materials.assign(box, crateId);
    world.step(1.0f / 60.0f);
    const auto impacts = traceBullet(world, materials, {0, 0, 0}, {0, 0, -1});
    CHECK(impacts.size() == 2);
    if (impacts.size() == 2) {
        CHECK(impacts[0].kind == Kind::Entry);
        CHECK(impacts[1].kind == Kind::Exit);
        CHECK_NEAR(impacts[1].point.z, -5.5f);
        CHECK_NEAR(impacts[1].energy, 1.0f - 0.04f * 2.0f);
    }
    world.shutdown();
}

static void unknown_bodies_stop_everything() {
    PhysicsWorld world;
    CHECK(world.init());
    PhysicalMaterials materials;
    world.createStaticBox({0, 0, -5}, {1, 1, 0.01f});
    world.step(1.0f / 60.0f);
    const auto impacts = traceBullet(world, materials, {0, 0, 0}, {0, 0, -1});
    CHECK(impacts.size() == 1 && impacts[0].kind == Kind::Stop);
    world.shutdown();
}

int main() {
    through_wood_into_concrete();
    glancing_off_steel();
    a_hollow_crate_costs_two_walls();
    unknown_bodies_stop_everything();
    return iron_test_result();
}
