#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraphIO.h"
#include "test_framework.h"

#include <string>

using namespace iron;

namespace {
// Build: Entry -> Branch(cond = Compare(a,b,op)) -> SetOutput(key,val).
// Returns outputs["r"] after a run.
float runBranchProgram(float a, float b, const char* op,
                       float trueVal, float falseVal) {
    NodeRegistry reg; registerBuiltinNodes(reg);
    Graph g;
    const NodeId entry = g.addNode("Entry");
    const NodeId cmp   = g.addNode("Compare");
    const NodeId br    = g.addNode("Branch");
    const NodeId setT  = g.addNode("SetOutput");
    const NodeId setF  = g.addNode("SetOutput");

    g.setLiteral(cmp, "a", NodeValue::F(a));
    g.setLiteral(cmp, "b", NodeValue::F(b));
    g.setLiteral(cmp, "op", NodeValue::S(op));
    g.connect(cmp, "result", br, "cond");
    g.connect(entry, "then", br, "in");

    g.setLiteral(setT, "key", NodeValue::S("r"));
    g.setLiteral(setT, "value", NodeValue::F(trueVal));
    g.setLiteral(setF, "key", NodeValue::S("r"));
    g.setLiteral(setF, "value", NodeValue::F(falseVal));
    g.connect(br, "true", setT, "in");
    g.connect(br, "false", setF, "in");

    RunContext ctx;
    run(g, reg, ctx);
    auto it = ctx.outputs.find("r");
    return it == ctx.outputs.end() ? -999.0f : it->second.asFloat();
}
}  // namespace

int main() {
    // NodeValue type + coercion.
    {
        CHECK(NodeValue::F(3.0f).type() == PortType::Float);
        CHECK_NEAR(NodeValue::I(5).asFloat(), 5.0f);
        CHECK(NodeValue::F(2.0f).asInt() == 2);
        CHECK(NodeValue::F(1.0f).asBool() == true);
        CHECK(NodeValue::B(true).asFloat() == 1.0f);
        CHECK(NodeValue::S("hi").asString() == std::string("hi"));
    }

    // portTypeName round-trips.
    {
        CHECK(std::string(portTypeName(PortType::Vec3)) == "Vec3");
        CHECK(portTypeFromName("Bool").value() == PortType::Bool);
        CHECK(!portTypeFromName("Nope").has_value());
    }

    // valueToJson/valueFromJson round-trip across all value types.
    {
        CHECK(valueFromJson(valueToJson(NodeValue::B(true))).asBool() == true);
        CHECK(valueFromJson(valueToJson(NodeValue::I(7))).asInt() == 7);
        CHECK_NEAR(valueFromJson(valueToJson(NodeValue::F(2.5f))).asFloat(), 2.5f);
        CHECK(valueFromJson(valueToJson(NodeValue::S("hi"))).asString() == std::string("hi"));
        const Vec2 v2 = valueFromJson(valueToJson(NodeValue::V2(Vec2{1, 2}))).asVec2();
        CHECK_NEAR(v2.x, 1.0f); CHECK_NEAR(v2.y, 2.0f);
        const Vec3 v3 = valueFromJson(valueToJson(NodeValue::V3(Vec3{1, 2, 3}))).asVec3();
        CHECK_NEAR(v3.x, 1.0f); CHECK_NEAR(v3.y, 2.0f); CHECK_NEAR(v3.z, 3.0f);
        const Vec4 v4 = valueFromJson(valueToJson(NodeValue::V4(Vec4{1, 2, 3, 4}))).asVec4();
        CHECK_NEAR(v4.x, 1.0f); CHECK_NEAR(v4.w, 4.0f);

        // zeroValue sanity.
        CHECK_NEAR(zeroValue(PortType::Float).asFloat(), 0.0f);
        CHECK(zeroValue(PortType::Bool).asBool() == false);

        // Malformed author JSON must NOT throw -> falls back to a zero value.
        const NodeValue bad = valueFromJson(nlohmann::json::parse(R"({"type":"Float"})"));
        CHECK_NEAR(bad.asFloat(), 0.0f);
    }

    // Graph build + queries.
    {
        Graph g;
        const NodeId a = g.addNode("Const");
        const NodeId b = g.addNode("SetOutput");
        CHECK(a == 1);
        CHECK(b == 2);
        g.setLiteral(a, "value", NodeValue::F(7.0f));
        g.connect(a, "out", b, "value");

        CHECK(g.nodes().size() == 2);
        CHECK(g.node(a)->literals.at("value").asFloat() == 7.0f);

        const auto inc = g.incoming(b, "value");
        CHECK(inc.has_value());
        CHECK(inc->fromNode == a);
        CHECK(inc->fromPort == "out");

        const auto out = g.outgoing(a, "out");
        CHECK(out.has_value());
        CHECK(out->toNode == b);

        CHECK(!g.incoming(b, "nope").has_value());
        CHECK(g.node(999) == nullptr);
    }

    // Registry + catalog introspection (the AI contract).
    {
        NodeRegistry reg;
        registerBuiltinNodes(reg);

        CHECK(reg.find("Branch") != nullptr);
        CHECK(reg.find("Nonexistent") == nullptr);
        CHECK(reg.all().size() == 7);

        const auto cat = catalogToJson(reg);
        CHECK(cat.is_array());
        CHECK(cat.size() == 7);
        bool foundBranch = false;
        for (const auto& n : cat) {
            if (n["typeName"] == "Branch") {
                foundBranch = true;
                bool hasCond = false, hasTrue = false;
                for (const auto& p : n["ports"]) {
                    if (p["name"] == "cond" && p["type"] == "Bool" && p["dir"] == "in") hasCond = true;
                    if (p["name"] == "true" && p["type"] == "Exec" && p["dir"] == "out") hasTrue = true;
                }
                CHECK(hasCond);
                CHECK(hasTrue);
            }
        }
        CHECK(foundBranch);
    }

    // Exec + data: true branch (7 > 5) writes 1.0; false branch writes 0.0.
    {
        CHECK_NEAR(runBranchProgram(7, 5, ">", 1.0f, 0.0f), 1.0f);
        CHECK_NEAR(runBranchProgram(2, 5, ">", 1.0f, 0.0f), 0.0f);
    }

    // Pure data pull: Add(Const 5, Const 3) -> SetOutput -> 8.
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        Graph g;
        const NodeId entry = g.addNode("Entry");
        const NodeId c5 = g.addNode("Const");
        const NodeId c3 = g.addNode("Const");
        const NodeId add = g.addNode("Add");
        const NodeId set = g.addNode("SetOutput");
        g.setLiteral(c5, "value", NodeValue::F(5.0f));
        g.setLiteral(c3, "value", NodeValue::F(3.0f));
        g.connect(c5, "out", add, "a");
        g.connect(c3, "out", add, "b");
        g.connect(add, "result", set, "value");
        g.setLiteral(set, "key", NodeValue::S("sum"));
        g.connect(entry, "then", set, "in");

        RunContext ctx;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("sum").asFloat(), 8.0f);
    }

    // Literal default used when an input is unconnected: Add(Const 10, b=4).
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        Graph g;
        const NodeId entry = g.addNode("Entry");
        const NodeId c10 = g.addNode("Const");
        const NodeId add = g.addNode("Add");
        const NodeId set = g.addNode("SetOutput");
        g.setLiteral(c10, "value", NodeValue::F(10.0f));
        g.connect(c10, "out", add, "a");
        g.setLiteral(add, "b", NodeValue::F(4.0f));   // unconnected -> literal
        g.connect(add, "result", set, "value");
        g.setLiteral(set, "key", NodeValue::S("x"));
        g.connect(entry, "then", set, "in");

        RunContext ctx;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("x").asFloat(), 14.0f);
    }

    // Sequence runs both outputs in order.
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        Graph g;
        const NodeId entry = g.addNode("Entry");
        const NodeId seq = g.addNode("Sequence");
        const NodeId s0 = g.addNode("SetOutput");
        const NodeId s1 = g.addNode("SetOutput");
        g.connect(entry, "then", seq, "in");
        g.connect(seq, "0", s0, "in");
        g.connect(seq, "1", s1, "in");
        g.setLiteral(s0, "key", NodeValue::S("a")); g.setLiteral(s0, "value", NodeValue::F(1.0f));
        g.setLiteral(s1, "key", NodeValue::S("b")); g.setLiteral(s1, "value", NodeValue::F(2.0f));
        RunContext ctx;
        run(g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("a").asFloat(), 1.0f);
        CHECK_NEAR(ctx.outputs.at("b").asFloat(), 2.0f);
    }

    // Infinite-loop guard: a Sequence wired back into itself halts (no hang).
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        Graph g;
        const NodeId entry = g.addNode("Entry");
        const NodeId seq = g.addNode("Sequence");
        g.connect(entry, "then", seq, "in");
        g.connect(seq, "0", seq, "in");  // cycle
        RunContext ctx;
        ctx.maxSteps = 100;
        run(g, reg, ctx);   // must return, not hang
        CHECK(true);
    }

    // JSON round-trip: build -> toJson -> fromJson -> re-run gives same output.
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        Graph g;
        const NodeId entry = g.addNode("Entry");
        const NodeId set = g.addNode("SetOutput");
        g.setLiteral(set, "key", NodeValue::S("v"));
        g.setLiteral(set, "value", NodeValue::F(42.0f));
        g.connect(entry, "then", set, "in");

        const nlohmann::json j = toJson(g);
        const auto g2 = fromJson(j, reg);
        CHECK(g2.has_value());
        CHECK(g2->nodes().size() == 2);
        CHECK(g2->connections().size() == 1);

        RunContext ctx;
        run(*g2, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("v").asFloat(), 42.0f);
    }

    // Author a graph as text (the AI path) and run it.
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        const char* text = R"JSON(
        {
          "nodes": [
            {"id":1,"type":"Entry"},
            {"id":2,"type":"SetOutput","literals":{
                "key":{"type":"String","value":"ai"},
                "value":{"type":"Float","value":99.0}}}
          ],
          "connections": [
            {"from":{"node":1,"port":"then"},"to":{"node":2,"port":"in"}}
          ]
        })JSON";
        const auto g = fromJson(nlohmann::json::parse(text), reg);
        CHECK(g.has_value());
        RunContext ctx;
        run(*g, reg, ctx);
        CHECK_NEAR(ctx.outputs.at("ai").asFloat(), 99.0f);
    }

    // Unknown node type fails the load loudly.
    {
        NodeRegistry reg; registerBuiltinNodes(reg);
        const char* text = R"JSON({"nodes":[{"id":1,"type":"Bogus"}],"connections":[]})JSON";
        const auto g = fromJson(nlohmann::json::parse(text), reg);
        CHECK(!g.has_value());
    }

    return iron_test_result();
}
