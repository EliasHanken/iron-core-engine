#include "gameplay/GameContext.h"
#include "gameplay/GameplayNodes.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "world/Parent.h"
#include "world/Transform.h"
#include "world/World.h"
#include "test_framework.h"

using namespace iron;

static NodeRegistry makeReg() {
    NodeRegistry r; registerBuiltinNodes(r); registerGameplayNodes(r);
    return r;
}

// Parent at +5 on X, child local +2 -> child world X = 7.
static void test_get_world_position_through_parent() {
    World w;
    EntityId parent = w.create();
    Transform pt; pt.position = {5, 0, 0}; w.add<Transform>(parent, pt);
    EntityId child = w.create();
    Transform ct; ct.position = {2, 0, 0}; w.add<Transform>(child, ct);
    w.add<Parent>(child, Parent{parent});

    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId gwp  = g.addNode("GetWorldPosition");
    const NodeId out  = g.addNode("SetOutput");
    g.setLiteral(out, "key", NodeValue::S("wp"));
    g.connect(gwp, "pos", out, "value");
    g.connect(tick, "then", out, "in");

    GameContext gc{&w, child, 0.0f, 0.0f};
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK_NEAR(ctx.outputs.at("wp").asVec3().x, 7.0f);
}

// Setting world {7,3,-1} on a child of a parent at {5,1,-2} stores local {2,2,1}.
static void test_set_world_position_through_parent() {
    World w;
    EntityId parent = w.create();
    Transform pt; pt.position = {5, 1, -2}; w.add<Transform>(parent, pt);
    EntityId child = w.create();
    Transform ct; ct.position = {0, 0, 0}; w.add<Transform>(child, ct);
    w.add<Parent>(child, Parent{parent});

    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId swp  = g.addNode("SetWorldPosition");
    g.setLiteral(swp, "pos", NodeValue::V3(Vec3{7, 3, -1}));
    g.connect(tick, "then", swp, "in");

    GameContext gc{&w, child, 0.0f, 0.0f};
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    const Vec3 p = w.get<Transform>(child)->position;
    CHECK_NEAR(p.x, 2.0f);   // 7 - 5
    CHECK_NEAR(p.y, 2.0f);   // 3 - 1
    CHECK_NEAR(p.z, 1.0f);   // -1 - (-2)
}

// On a root (no Parent), set world == local.
static void test_set_world_position_root() {
    World w;
    EntityId e = w.create(); w.add<Transform>(e, Transform{});
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId swp  = g.addNode("SetWorldPosition");
    g.setLiteral(swp, "pos", NodeValue::V3(Vec3{9, 0, 0}));
    g.connect(tick, "then", swp, "in");

    GameContext gc{&w, e, 0.0f, 0.0f};
    RunContext ctx; ctx.domainContext = &gc;
    run(g, reg, ctx);
    CHECK_NEAR(w.get<Transform>(e)->position.x, 9.0f);
}

// Editor preview: no domainContext -> world-space nodes no-op, no crash.
static void test_world_nodes_null_context_no_crash() {
    NodeRegistry reg = makeReg();
    Graph g;
    const NodeId tick = g.addNode("OnTick");
    const NodeId gwp  = g.addNode("GetWorldPosition");
    const NodeId swp  = g.addNode("SetWorldPosition");
    const NodeId out  = g.addNode("SetOutput");
    g.setLiteral(out, "key", NodeValue::S("wp"));
    g.connect(gwp, "pos", out, "value");
    g.connect(tick, "then", swp, "in");
    g.connect(swp, "then", out, "in");
    RunContext ctx;            // domainContext == nullptr
    run(g, reg, ctx);          // must not crash
    CHECK(true);
}

int main() {
    test_get_world_position_through_parent();
    test_set_world_position_through_parent();
    test_set_world_position_root();
    test_world_nodes_null_context_no_crash();
    return iron_test_result();
}
