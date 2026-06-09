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

struct Decoy { float power = -1.0f; };   // same field name, different component

void registerDecoy(Reflection& r) {
    r.registerType<Decoy>("Decoy").field("power", &Decoy::power);
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
    registerDecoy(r);
    ComponentRegistry cr;
    cr.registerComponent<Combat>("Combat", r);
    cr.registerComponent<Decoy>("Decoy", r);

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
        cs.add<Decoy>();              // first box: wrong component, same field name
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

    // Set nodes exist for exactly the writable supported fields.
    {
        CHECK(reg.find("Set Combat power") != nullptr);
        CHECK(reg.find("Set Combat ammo")  != nullptr);
        CHECK(reg.find("Set Combat armed") != nullptr);
        CHECK(reg.find("Set Combat aim")   != nullptr);
        CHECK(reg.find("Set Combat label") != nullptr);
        CHECK(reg.find("Set Combat score")  == nullptr);   // readOnly
        CHECK(reg.find("Set Combat secret") == nullptr);   // hidden
        CHECK(reg.find("Set Combat facing") == nullptr);   // unsupported Quat
        const NodeTypeDesc* d = reg.find("Set Combat aim");
        CHECK(d->category == "Components");
        CHECK(d->ports.size() == 3u);   // in / value / then
        CHECK(findPort(d, "in")    && findPort(d, "in")->type    == PortType::Exec);
        CHECK(findPort(d, "value") && findPort(d, "value")->type == PortType::Vec3);
        CHECK(findPort(d, "then")  && findPort(d, "then")->type  == PortType::Exec);
    }

    // Set-then-Get round-trip through the evaluator, per supported type.
    // Chain: OnTick -> Set power -> Set ammo -> Set armed -> Set aim ->
    // Set label -> SetOutput(reads Get Combat power post-write).
    {
        World w; EntityId e = w.create();
        ComponentSet cs;
        cs.add<Decoy>();   // first box: same field name — write must discriminate by typeId
        cs.add<Combat>();
        w.add<ComponentSet>(e, cs);

        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId sP = g.addNode("Set Combat power");
        const NodeId sA = g.addNode("Set Combat ammo");
        const NodeId sB = g.addNode("Set Combat armed");
        const NodeId sV = g.addNode("Set Combat aim");
        const NodeId sS = g.addNode("Set Combat label");
        const NodeId getN = g.addNode("Get Combat");
        const NodeId out  = g.addNode("SetOutput");
        g.setLiteral(sP, "value", NodeValue::F(42.0f));
        g.setLiteral(sA, "value", NodeValue::I(9));
        g.setLiteral(sB, "value", NodeValue::B(false));
        g.setLiteral(sV, "value", NodeValue::V3(Vec3{1.0f, 2.0f, 3.0f}));
        g.setLiteral(sS, "value", NodeValue::S("sword"));
        g.setLiteral(out, "key", NodeValue::S("p"));
        g.connect(getN, "power", out, "value");
        g.connect(tick, "then", sP, "in");
        g.connect(sP, "then", sA, "in");
        g.connect(sA, "then", sB, "in");
        g.connect(sB, "then", sV, "in");
        g.connect(sV, "then", sS, "in");
        g.connect(sS, "then", out, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);

        // Get pulled AFTER the writes in the exec chain sees the new value.
        CHECK_NEAR(ctx.outputs.at("p").asFloat(), 42.0f);
        // And the runtime World copy holds every written value.
        Combat* c = w.get<ComponentSet>(e)->get<Combat>();
        CHECK(c != nullptr);
        CHECK_NEAR(c->power, 42.0f);
        CHECK(c->ammo == 9);
        CHECK(!c->armed);
        CHECK_NEAR(c->aim.x, 1.0f);
        CHECK_NEAR(c->aim.y, 2.0f);
        CHECK_NEAR(c->aim.z, 3.0f);
        CHECK(c->label == "sword");
        // readOnly + hidden + unsupported fields untouched (no Set node exists).
        CHECK_NEAR(c->score, 1.5f);
        CHECK_NEAR(c->secret, 9.0f);
        CHECK_NEAR(w.get<ComponentSet>(e)->get<Decoy>()->power, -1.0f);   // untouched
    }

    // Missing component: Set is a silent no-op and exec continues past it.
    {
        World w; EntityId e = w.create();   // no ComponentSet
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId sP   = g.addNode("Set Combat power");
        const NodeId out  = g.addNode("SetOutput");
        g.setLiteral(sP, "value", NodeValue::F(5.0f));
        g.setLiteral(out, "key", NodeValue::S("ran"));
        g.setLiteral(out, "value", NodeValue::F(1.0f));
        g.connect(tick, "then", sP, "in");
        g.connect(sP, "then", out, "in");

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("ran").asFloat(), 1.0f);   // chain didn't stall
    }

    // Null domainContext: Set no-ops without crashing, exec continues.
    {
        Graph g;
        const NodeId tick = g.addNode("OnTick");
        const NodeId sP   = g.addNode("Set Combat power");
        const NodeId out  = g.addNode("SetOutput");
        g.setLiteral(sP, "value", NodeValue::F(5.0f));
        g.setLiteral(out, "key", NodeValue::S("ran"));
        g.setLiteral(out, "value", NodeValue::F(1.0f));
        g.connect(tick, "then", sP, "in");
        g.connect(sP, "then", out, "in");
        RunContext ctx;                      // domainContext == nullptr
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("ran").asFloat(), 1.0f);
    }

    // Generated nodes are part of the AI contract (catalogToJson).
    {
        const std::string cat = catalogToJson(reg).dump();
        CHECK(cat.find("Get Combat") != std::string::npos);
        CHECK(cat.find("Set Combat power") != std::string::npos);
        CHECK(cat.find("Set Combat score") == std::string::npos);   // readOnly
    }

    // Memoization pin: a Get node pulled BEFORE a Set keeps its pre-write
    // value on later pulls in the same run; a SEPARATE Get node re-reads.
    // (Evaluator memoizes per node+port per run — documented in
    // ComponentNodes.h; this test makes any future change deliberate.)
    // Wiring note: Sequence only has outputs "0"/"1", and SetOutput is a sink
    // (no exec-out). We use two nested Sequences:
    //   tick -> seqOuter -> "0" -> seqInner -> "0" -> outA0 (getA pre-write)
    //                                       -> "1" -> sP (writes 42)
    //                                sP "then" -> outA1 (getA memoized, stale)
    //              seqOuter "1" -> outB (getB fresh, re-reads 42)
    {
        World w; EntityId e = w.create();
        ComponentSet cs; cs.add<Combat>();          // power = 10
        w.add<ComponentSet>(e, cs);

        Graph g;
        const NodeId tick     = g.addNode("OnTick");
        const NodeId getA     = g.addNode("Get Combat");   // pulled pre-write
        const NodeId getB     = g.addNode("Get Combat");   // pulled post-write
        const NodeId outA0    = g.addNode("SetOutput");    // getA before Set
        const NodeId sP       = g.addNode("Set Combat power");
        const NodeId outA1    = g.addNode("SetOutput");    // getA after Set (memoized)
        const NodeId outB     = g.addNode("SetOutput");    // getB after Set (fresh)
        const NodeId seqOuter = g.addNode("Sequence");
        const NodeId seqInner = g.addNode("Sequence");
        g.setLiteral(sP,    "value", NodeValue::F(42.0f));
        g.setLiteral(outA0, "key",   NodeValue::S("a0"));
        g.setLiteral(outA1, "key",   NodeValue::S("a1"));
        g.setLiteral(outB,  "key",   NodeValue::S("b"));
        // data wiring
        g.connect(getA, "power", outA0, "value");
        g.connect(getA, "power", outA1, "value");
        g.connect(getB, "power", outB,  "value");
        // exec wiring
        g.connect(tick,     "then", seqOuter, "in");
        g.connect(seqOuter, "0",    seqInner, "in");
        g.connect(seqInner, "0",    outA0,    "in");   // fires getA (first pull)
        g.connect(seqInner, "1",    sP,       "in");   // writes 42
        g.connect(sP,       "then", outA1,    "in");   // getA memoized -> stale
        g.connect(seqOuter, "1",    outB,     "in");   // getB fresh -> 42

        GameContext gc{&w, e, 0.0f, 0.016f};
        RunContext ctx; ctx.domainContext = &gc;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("a0").asFloat(), 10.0f);   // pre-write
        CHECK_NEAR(ctx.outputs.at("a1").asFloat(), 10.0f);   // memoized (stale)
        CHECK_NEAR(ctx.outputs.at("b").asFloat(),  42.0f);   // fresh node re-reads
    }

    return iron_test_result();
}
