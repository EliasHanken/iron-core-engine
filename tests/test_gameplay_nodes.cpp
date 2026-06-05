#include "gameplay/GameContext.h"
#include "gameplay/GameplayNodes.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "world/Transform.h"
#include "world/World.h"
#include "test_framework.h"

#include <cmath>

using namespace iron;

namespace {
NodeRegistry makeReg() {
    NodeRegistry r; registerBuiltinNodes(r); registerGameplayNodes(r);
    return r;
}
}  // namespace

int main() {
    // OnTick -> Translate moves the self entity's position by the delta.
    {
        World w; EntityId e = w.create(); w.add<Transform>(e, Transform{});
        NodeRegistry reg = makeReg();
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId mk   = g.addNode("MakeVec3");
        const NodeId tr   = g.addNode("Translate");
        g.setLiteral(mk, "x", NodeValue::F(2.0f));
        g.connect(mk, "v", tr, "delta");
        g.connect(tick, "then", tr, "in");

        GameContext gc{&w, e, 0.0f, 0.1f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        CHECK_NEAR(w.get<Transform>(e)->position.x, 2.0f);
    }

    // GetPosition/BreakVec3/Sin path: set Y to sin(time); preserve X,Z.
    {
        World w; EntityId e = w.create();
        Transform t0; t0.position = Vec3{1.0f, 0.0f, 3.0f};
        w.add<Transform>(e, t0);
        NodeRegistry reg = makeReg();
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId getp = g.addNode("GetPosition");
        const NodeId br   = g.addNode("BreakVec3");
        const NodeId sinN = g.addNode("Sin");
        const NodeId mk   = g.addNode("MakeVec3");
        const NodeId setp = g.addNode("SetPosition");
        g.connect(getp, "pos", br, "v");
        g.connect(tick, "time", sinN, "x");
        g.connect(br, "x", mk, "x");
        g.connect(sinN, "result", mk, "y");
        g.connect(br, "z", mk, "z");
        g.connect(mk, "v", setp, "pos");
        g.connect(tick, "then", setp, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        const Vec3 p = w.get<Transform>(e)->position;
        CHECK_NEAR(p.x, 1.0f);
        CHECK_NEAR(p.y, 0.0f);   // sin(0)
        CHECK_NEAR(p.z, 3.0f);
    }

    // SetVar then GetVar round-trips through the persistent vars blackboard.
    {
        World w; EntityId e = w.create(); w.add<Transform>(e, Transform{});
        NodeRegistry reg = makeReg();
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId setv = g.addNode("SetVar");
        const NodeId getv = g.addNode("GetVar");
        const NodeId out  = g.addNode("SetOutput");
        const NodeId seq  = g.addNode("Sequence");
        g.setLiteral(setv, "name", NodeValue::S("hp"));
        g.setLiteral(setv, "value", NodeValue::F(42.0f));
        g.setLiteral(getv, "name", NodeValue::S("hp"));
        g.setLiteral(out, "key", NodeValue::S("r"));
        g.connect(getv, "value", out, "value");
        g.connect(tick, "then", seq, "in");
        g.connect(seq, "0", setv, "in");
        g.connect(seq, "1", out, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("r").asFloat(), 42.0f);
    }

    // No domainContext (editor preview): gameplay nodes no-op, no crash.
    {
        NodeRegistry reg = makeReg();
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId tr   = g.addNode("Translate");
        g.setLiteral(tr, "delta", NodeValue::V3(Vec3{1, 1, 1}));
        g.connect(tick, "then", tr, "in");
        RunContext ctx;                      // domainContext == nullptr
        run(g, reg, ctx);                    // must not crash
        CHECK(true);
    }

    return iron_test_result();
}
