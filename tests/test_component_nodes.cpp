#include "gameplay/ComponentNodes.h"
#include "gameplay/GameContext.h"
#include "gameplay/GameplayNodes.h"
#include "math/Quaternion.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "reflection/Reflection.h"
#include "world/ComponentRegistry.h"
#include "world/ComponentSet.h"
#include "world/World.h"
#include "test_framework.h"

#include <string>

using namespace iron;

namespace {

// One field per supported TypeId + a readOnly + a hidden + an unsupported one.
struct Combat {
    float       power  = 10.0f;
    int         ammo   = 3;
    bool        armed  = true;
    Vec3        aim    = Vec3{0.0f, 0.0f, 1.0f};
    std::string label  = "gun";
    float       score  = 1.5f;   // readOnly -> Get pin, no Set node
    float       secret = 9.0f;   // hidden   -> no pins, no nodes
    Quat        facing = {};     // Quat unsupported -> no pin, no node
};

void registerCombat(Reflection& r) {
    r.registerType<Combat>("Combat")
        .field("power",  &Combat::power)
        .field("ammo",   &Combat::ammo)
        .field("armed",  &Combat::armed)
        .field("aim",    &Combat::aim)
        .field("label",  &Combat::label)
        .field("score",  &Combat::score,  {.readOnly = true})
        .field("secret", &Combat::secret, {.hidden = true})
        .field("facing", &Combat::facing);
}

const PortDesc* findPort(const NodeTypeDesc* d, std::string_view name) {
    for (const PortDesc& p : d->ports)
        if (p.name == name) return &p;
    return nullptr;
}

}  // namespace

int main() {
    Reflection r;
    registerCombat(r);
    ComponentRegistry cr;
    cr.registerComponent<Combat>("Combat", r);

    NodeRegistry reg;
    registerBuiltinNodes(reg);
    registerGameplayNodes(reg);
    registerComponentNodes(reg, cr);

    // Get node exists with `has` + exactly the supported, non-hidden fields.
    {
        const NodeTypeDesc* d = reg.find("Get Combat");
        CHECK(d != nullptr);
        CHECK(d->category == "Components");
        CHECK(d->ports.size() == 7u);   // has + power/ammo/armed/aim/label/score
        CHECK(findPort(d, "has")   && findPort(d, "has")->type   == PortType::Bool);
        CHECK(findPort(d, "power") && findPort(d, "power")->type == PortType::Float);
        CHECK(findPort(d, "ammo")  && findPort(d, "ammo")->type  == PortType::Int);
        CHECK(findPort(d, "armed") && findPort(d, "armed")->type == PortType::Bool);
        CHECK(findPort(d, "aim")   && findPort(d, "aim")->type   == PortType::Vec3);
        CHECK(findPort(d, "label") && findPort(d, "label")->type == PortType::String);
        CHECK(findPort(d, "score") && findPort(d, "score")->type == PortType::Float);
        CHECK(findPort(d, "secret") == nullptr);   // hidden
        CHECK(findPort(d, "facing") == nullptr);   // unsupported Quat
        for (const PortDesc& p : d->ports) CHECK(p.dir == PortDir::Out);
    }

    // Get reads live values from the entity's runtime ComponentSet.
    {
        World w; EntityId e = w.create();
        ComponentSet cs;
        Combat c0; c0.power = 77.0f;
        cs.add<Combat>(c0);
        w.add<ComponentSet>(e, cs);

        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId getN = g.addNode("Get Combat");
        const NodeId out  = g.addNode("SetOutput");
        g.setLiteral(out, "key", NodeValue::S("p"));
        g.connect(getN, "power", out, "value");
        g.connect(tick, "then", out, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("p").asFloat(), 77.0f);
    }

    // Missing component: has=false, field pins read type-appropriate zeros.
    {
        World w; EntityId e = w.create();   // no ComponentSet at all
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId getN = g.addNode("Get Combat");
        const NodeId outH = g.addNode("SetOutput");
        const NodeId outP = g.addNode("SetOutput");
        const NodeId seq  = g.addNode("Sequence");
        g.setLiteral(outH, "key", NodeValue::S("has"));
        g.setLiteral(outP, "key", NodeValue::S("p"));
        g.connect(getN, "has",   outH, "value");
        g.connect(getN, "power", outP, "value");
        g.connect(tick, "then", seq, "in");
        g.connect(seq, "0", outH, "in");
        g.connect(seq, "1", outP, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        CHECK(!ctx.outputs.at("has").asBool());
        CHECK_NEAR(ctx.outputs.at("p").asFloat(), 0.0f);
    }

    // Null domainContext (editor Run preview): no crash, has=false.
    {
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId getN = g.addNode("Get Combat");
        const NodeId out  = g.addNode("SetOutput");
        g.setLiteral(out, "key", NodeValue::S("has"));
        g.connect(getN, "has", out, "value");
        g.connect(tick, "then", out, "in");
        RunContext ctx;                      // domainContext == nullptr
        run(g, reg, ctx);                    // must not crash
        CHECK(!ctx.outputs.at("has").asBool());
    }

    return iron_test_result();
}
