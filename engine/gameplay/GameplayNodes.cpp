#include "gameplay/GameplayNodes.h"

#include "gameplay/GameContext.h"
#include "nodes/NodeContext.h"
#include "nodes/NodeRegistry.h"
#include "world/Transform.h"
#include "world/World.h"

#include <cmath>

namespace iron {

namespace {
GameContext* gameOf(NodeContext& c) {
    return static_cast<GameContext*>(c.run().domainContext);
}
Transform* selfTransform(NodeContext& c) {
    GameContext* g = gameOf(c);
    if (!g || !g->world) return nullptr;
    return g->world->get<Transform>(g->self);
}
}  // namespace

void registerGameplayNodes(NodeRegistry& r) {
    using P = PortDesc;
    const auto In  = PortDir::In;
    const auto Out = PortDir::Out;

    r.registerType({"OnTick", "Event",
        { P{"then", PortType::Exec, Out}, P{"dt", PortType::Float, Out},
          P{"time", PortType::Float, Out} },
        [](NodeContext& c) {
            GameContext* g = gameOf(c);
            c.out("dt",   NodeValue::F(g ? g->deltaTime : 0.0f));
            c.out("time", NodeValue::F(g ? g->time : 0.0f));
            c.fire("then");
        }, true, "Every frame"});

    r.registerType({"GetPosition", "Transform",
        { P{"pos", PortType::Vec3, Out} },
        [](NodeContext& c) {
            Transform* t = selfTransform(c);
            c.out("pos", NodeValue::V3(t ? t->position : Vec3{0, 0, 0}));
        }, false, "Self Transform position"});

    r.registerType({"SetPosition", "Transform",
        { P{"in", PortType::Exec, In}, P{"pos", PortType::Vec3, In},
          P{"then", PortType::Exec, Out} },
        [](NodeContext& c) {
            if (Transform* t = selfTransform(c)) t->position = c.in("pos").asVec3();
            c.fire("then");
        }, false, "Set self Transform position"});

    r.registerType({"Translate", "Transform",
        { P{"in", PortType::Exec, In}, P{"delta", PortType::Vec3, In},
          P{"then", PortType::Exec, Out} },
        [](NodeContext& c) {
            if (Transform* t = selfTransform(c))
                t->position = t->position + c.in("delta").asVec3();
            c.fire("then");
        }, false, "Offset position by delta"});

    r.registerType({"MakeVec3", "Math",
        { P{"x", PortType::Float, In}, P{"y", PortType::Float, In},
          P{"z", PortType::Float, In}, P{"v", PortType::Vec3, Out} },
        [](NodeContext& c) {
            c.out("v", NodeValue::V3(Vec3{c.in("x").asFloat(), c.in("y").asFloat(),
                                          c.in("z").asFloat()}));
        }, false, "x, y, z -> Vec3"});

    r.registerType({"BreakVec3", "Math",
        { P{"v", PortType::Vec3, In}, P{"x", PortType::Float, Out},
          P{"y", PortType::Float, Out}, P{"z", PortType::Float, Out} },
        [](NodeContext& c) {
            const Vec3 v = c.in("v").asVec3();
            c.out("x", NodeValue::F(v.x));
            c.out("y", NodeValue::F(v.y));
            c.out("z", NodeValue::F(v.z));
        }, false, "Vec3 -> x, y, z"});

    r.registerType({"Mul", "Math",
        { P{"a", PortType::Float, In}, P{"b", PortType::Float, In},
          P{"result", PortType::Float, Out} },
        [](NodeContext& c) {
            c.out("result", NodeValue::F(c.in("a").asFloat() * c.in("b").asFloat()));
        }, false, "a * b"});

    r.registerType({"Sin", "Math",
        { P{"x", PortType::Float, In}, P{"result", PortType::Float, Out} },
        [](NodeContext& c) {
            c.out("result", NodeValue::F(std::sin(c.in("x").asFloat())));
        }, false, "sin(x) radians"});

    r.registerType({"GetVar", "Variable",
        { P{"name", PortType::String, In}, P{"value", PortType::Float, Out} },
        [](NodeContext& c) {
            auto& vars = c.run().vars;
            auto it = vars.find(c.in("name").asString());
            c.out("value", it != vars.end() ? it->second : NodeValue::F(0.0f));
        }, false, "Read a graph variable"});

    r.registerType({"SetVar", "Variable",
        { P{"in", PortType::Exec, In}, P{"name", PortType::String, In},
          P{"value", PortType::Float, In}, P{"then", PortType::Exec, Out} },
        [](NodeContext& c) {
            c.run().vars[c.in("name").asString()] = c.in("value");
            c.fire("then");
        }, false, "Write a graph variable"});
}

}  // namespace iron
