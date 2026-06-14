#include "gameplay/GameContext.h"
#include "gameplay/GameplayNodes.h"
#include "gameplay/SpawnPoint.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "world/ComponentSet.h"
#include "world/Transform.h"
#include "world/World.h"
#include "test_framework.h"

#include <cmath>
#include <cstdint>

using namespace iron;

static NodeRegistry makeReg() {
    NodeRegistry r; registerBuiltinNodes(r); registerGameplayNodes(r);
    return r;
}

// A spawn point as the runtime sees it: a Transform + a ComponentSet carrying a
// SpawnPoint (matching how spawnRuntime mirrors authored components).
static EntityId makeMarker(World& w, Vec3 pos, std::string group, bool enabled) {
    EntityId e = w.create();
    Transform t; t.position = pos; w.add<Transform>(e, t);
    ComponentSet cs; cs.add<SpawnPoint>(SpawnPoint{std::move(group), enabled});
    w.add<ComponentSet>(e, cs);
    return e;
}

static void test_get_spawn_point_first_enabled() {
    World w;
    makeMarker(w, {1, 0, 0}, "enemy", true);
    makeMarker(w, {2, 0, 0}, "enemy", true);
    makeMarker(w, {9, 9, 9}, "player", true);   // wrong group

    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId gsp  = g.addNode("GetSpawnPoint");
    const NodeId seq  = g.addNode("Sequence");
    const NodeId oPos = g.addNode("SetOutput");
    const NodeId oFnd = g.addNode("SetOutput");
    g.setLiteral(gsp, "group", NodeValue::S("enemy"));
    g.setLiteral(oPos, "key", NodeValue::S("pos"));
    g.setLiteral(oFnd, "key", NodeValue::S("found"));
    g.connect(gsp, "pos",   oPos, "value");
    g.connect(gsp, "found", oFnd, "value");
    g.connect(tick, "then", seq, "in");
    g.connect(seq, "0", oPos, "in");
    g.connect(seq, "1", oFnd, "in");

    GameContext gc{&w, EntityId{}, 0.0f, 0.0f};
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK(ctx.outputs.at("found").asBool() == true);
    CHECK_NEAR(ctx.outputs.at("pos").asVec3().x, 1.0f);   // first enabled enemy
}

static void test_get_spawn_point_none_found() {
    World w;
    makeMarker(w, {1, 0, 0}, "enemy", false);   // disabled
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId gsp  = g.addNode("GetSpawnPoint");
    const NodeId oFnd = g.addNode("SetOutput");
    g.setLiteral(gsp, "group", NodeValue::S("enemy"));
    g.setLiteral(oFnd, "key", NodeValue::S("found"));
    g.connect(gsp, "found", oFnd, "value");
    g.connect(tick, "then", oFnd, "in");

    GameContext gc{&w, EntityId{}, 0.0f, 0.0f};
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK(ctx.outputs.at("found").asBool() == false);
}

static void test_get_random_spawn_point_member_of_group() {
    World w;
    makeMarker(w, {1, 0, 0}, "enemy", true);
    makeMarker(w, {2, 0, 0}, "enemy", true);
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId grp  = g.addNode("GetRandomSpawnPoint");
    const NodeId oPos = g.addNode("SetOutput");
    g.setLiteral(grp, "group", NodeValue::S("enemy"));
    g.setLiteral(oPos, "key", NodeValue::S("pos"));
    g.connect(grp, "pos", oPos, "value");
    g.connect(tick, "then", oPos, "in");

    std::uint32_t seed = 12345u;
    GameContext gc{&w, EntityId{}, 0.0f, 0.0f};
    gc.rngState = &seed;
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    const float x = ctx.outputs.at("pos").asVec3().x;
    CHECK(x == 1.0f || x == 2.0f);   // one of the group's markers
}

static void test_get_random_spawn_point_null_rng_returns_member() {
    World w;
    makeMarker(w, {1, 0, 0}, "enemy", true);
    makeMarker(w, {2, 0, 0}, "enemy", true);
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId grp  = g.addNode("GetRandomSpawnPoint");
    const NodeId oPos = g.addNode("SetOutput");
    const NodeId oFnd = g.addNode("SetOutput");
    const NodeId seq  = g.addNode("Sequence");
    g.setLiteral(grp, "group", NodeValue::S("enemy"));
    g.setLiteral(oPos, "key", NodeValue::S("pos"));
    g.setLiteral(oFnd, "key", NodeValue::S("found"));
    g.connect(grp, "pos",   oPos, "value");
    g.connect(grp, "found", oFnd, "value");
    g.connect(tick, "then", seq, "in");
    g.connect(seq, "0", oPos, "in");
    g.connect(seq, "1", oFnd, "in");

    GameContext gc{&w, EntityId{}, 0.0f, 0.0f};   // rngState == nullptr
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK(ctx.outputs.at("found").asBool() == true);
    const float x = ctx.outputs.at("pos").asVec3().x;
    CHECK(x == 1.0f || x == 2.0f);
}

static void test_spawn_prefab_enqueues() {
    World w;
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId sp   = g.addNode("SpawnPrefab");
    g.setLiteral(sp, "prefab", NodeValue::S("enemy.prefab"));
    g.setLiteral(sp, "pos",    NodeValue::V3(Vec3{1, 2, 3}));
    g.connect(tick, "then", sp, "in");

    std::vector<SpawnRequest> queue;
    GameContext gc{&w, EntityId{}, 0.0f, 0.0f};
    gc.spawnQueue = &queue;
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK(queue.size() == 1);
    CHECK(queue.size() == 1 && queue[0].prefabPath == "enemy.prefab");
    CHECK(queue.size() == 1 && std::fabs(queue[0].position.x - 1.0f) < 1e-4f);
    CHECK(queue.size() == 1 && std::fabs(queue[0].position.z - 3.0f) < 1e-4f);
}

static void test_spawn_prefab_null_queue_is_safe() {
    World w;
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId sp   = g.addNode("SpawnPrefab");
    g.setLiteral(sp, "prefab", NodeValue::S("x.prefab"));
    g.connect(tick, "then", sp, "in");
    GameContext gc{&w, EntityId{}, 0.0f, 0.0f};   // spawnQueue == nullptr
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);                              // must not crash
    CHECK(true);
}

int main() {
    test_get_spawn_point_first_enabled();
    test_get_spawn_point_none_found();
    test_get_random_spawn_point_member_of_group();
    test_get_random_spawn_point_null_rng_returns_member();
    test_spawn_prefab_enqueues();
    test_spawn_prefab_null_queue_is_safe();
    return iron_test_result();
}
