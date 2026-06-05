#include "nodes/NodeGraph.h"
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

    // valueToJson/valueFromJson round-trip (scalar + vector).
    {
        const NodeValue a = NodeValue::F(2.5f);
        CHECK_NEAR(valueFromJson(valueToJson(a)).asFloat(), 2.5f);
        const NodeValue b = NodeValue::V3(Vec3{1, 2, 3});
        const Vec3 r = valueFromJson(valueToJson(b)).asVec3();
        CHECK_NEAR(r.x, 1.0f); CHECK_NEAR(r.y, 2.0f); CHECK_NEAR(r.z, 3.0f);
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

    return iron_test_result();
}
