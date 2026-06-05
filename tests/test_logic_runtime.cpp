#include "gameplay/GameplayNodes.h"
#include "gameplay/LogicGraph.h"
#include "gameplay/LogicRuntime.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "world/Transform.h"
#include "world/World.h"
#include "test_framework.h"

using namespace iron;

int main() {
    NodeRegistry reg; registerBuiltinNodes(reg); registerGameplayNodes(reg);

    // OnTick -> Translate by (1,0,0) each tick. After 5 ticks, position.x == 5.
    {
        World w; EntityId e = w.create(); w.add<Transform>(e, Transform{});
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId tr   = g.addNode("Translate");
        g.setLiteral(tr, "delta", NodeValue::V3(Vec3{1.0f, 0.0f, 0.0f}));
        g.connect(tick, "then", tr, "in");
        w.add<LogicGraph>(e, LogicGraph{g, {}, false});

        for (int i = 0; i < 5; ++i) tickLogicGraphs(w, reg, i * 0.1f, 0.1f);
        CHECK_NEAR(w.get<Transform>(e)->position.x, 5.0f);
    }

    // Persistent variable accumulates: SetVar("n", GetVar("n")+1) each tick.
    {
        World w; EntityId e = w.create(); w.add<Transform>(e, Transform{});
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId getv = g.addNode("GetVar");
        const NodeId add  = g.addNode("Add");
        const NodeId setv = g.addNode("SetVar");
        g.setLiteral(getv, "name", NodeValue::S("n"));
        g.setLiteral(add, "b", NodeValue::F(1.0f));
        g.setLiteral(setv, "name", NodeValue::S("n"));
        g.connect(getv, "value", add, "a");
        g.connect(add, "result", setv, "value");
        g.connect(tick, "then", setv, "in");
        w.add<LogicGraph>(e, LogicGraph{g, {}, false});

        for (int i = 0; i < 3; ++i) tickLogicGraphs(w, reg, 0.0f, 0.1f);
        const LogicGraph* lg = w.get<LogicGraph>(e);
        CHECK_NEAR(lg->vars.at("n").asFloat(), 3.0f);
    }

    return iron_test_result();
}
