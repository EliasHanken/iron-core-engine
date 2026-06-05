#include "nodes/BuiltinNodes.h"

#include "nodes/NodeContext.h"
#include "nodes/NodeRegistry.h"

namespace iron {

void registerBuiltinNodes(NodeRegistry& r) {
    using P = PortDesc;
    const auto In  = PortDir::In;
    const auto Out = PortDir::Out;

    // Entry: kicks off control flow.
    r.registerType({"Entry", "Flow",
        { P{"then", PortType::Exec, Out} },
        [](NodeContext& c) { c.fire("then"); }, true});

    // Branch: fire "true" or "false" based on the bool input.
    r.registerType({"Branch", "Flow",
        { P{"in", PortType::Exec, In}, P{"cond", PortType::Bool, In},
          P{"true", PortType::Exec, Out}, P{"false", PortType::Exec, Out} },
        [](NodeContext& c) { c.fire(c.in("cond").asBool() ? "true" : "false"); }});

    // Sequence: fire "0" then "1" in order (DFS in the evaluator).
    r.registerType({"Sequence", "Flow",
        { P{"in", PortType::Exec, In},
          P{"0", PortType::Exec, Out}, P{"1", PortType::Exec, Out} },
        [](NodeContext& c) { c.fire("0"); c.fire("1"); }});

    // Const: forward its literal-defaulted "value" input to "out".
    r.registerType({"Const", "Value",
        { P{"value", PortType::Float, In}, P{"out", PortType::Float, Out} },
        [](NodeContext& c) { c.out("out", c.in("value")); }});

    // Compare: a (op) b -> bool. op is a String input with a literal default.
    r.registerType({"Compare", "Math",
        { P{"a", PortType::Float, In}, P{"b", PortType::Float, In},
          P{"op", PortType::String, In}, P{"result", PortType::Bool, Out} },
        [](NodeContext& c) {
            const float a = c.in("a").asFloat();
            const float b = c.in("b").asFloat();
            const std::string op = c.in("op").asString();
            bool res = false;
            if (op == ">")       res = a > b;
            else if (op == "<")  res = a < b;
            else if (op == ">=") res = a >= b;
            else if (op == "<=") res = a <= b;
            else if (op == "==") res = a == b;
            c.out("result", NodeValue::B(res));
        }});

    // Add: a + b -> float.
    r.registerType({"Add", "Math",
        { P{"a", PortType::Float, In}, P{"b", PortType::Float, In},
          P{"result", PortType::Float, Out} },
        [](NodeContext& c) {
            c.out("result", NodeValue::F(c.in("a").asFloat() + c.in("b").asFloat()));
        }});

    // SetOutput (sink): write run().outputs[key] = value on exec.
    r.registerType({"SetOutput", "Sink",
        { P{"in", PortType::Exec, In}, P{"key", PortType::String, In},
          P{"value", PortType::Float, In} },
        [](NodeContext& c) {
            c.run().outputs[c.in("key").asString()] = c.in("value");
        }});
}

}  // namespace iron
