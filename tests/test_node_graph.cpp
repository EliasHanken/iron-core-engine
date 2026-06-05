#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "nodes/BuiltinNodes.h"
#include "test_framework.h"

#include <string>

using namespace iron;

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

    return iron_test_result();
}
