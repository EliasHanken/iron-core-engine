#include "nodes/GraphEditorModel.h"
#include "nodes/NodeRegistry.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/NodeContext.h"
#include "test_framework.h"

using namespace iron;

int main() {
    NodeRegistry reg; registerBuiltinNodes(reg);

    // addNode + position + dirty.
    {
        GraphEditorModel m(&reg);
        const NodeId id = m.addNode("Const", 10.0f, 20.0f);
        CHECK(m.graph().node(id) != nullptr);
        CHECK_NEAR(m.graph().node(id)->editorX, 10.0f);
        CHECK(m.dirty());
    }

    // connect validates types + Out->In; rejects mismatches; deleteNode clears wires.
    {
        GraphEditorModel m(&reg);
        const NodeId c = m.addNode("Const", 0, 0);
        const NodeId add = m.addNode("Add", 0, 0);
        const NodeId entry = m.addNode("Entry", 0, 0);

        CHECK(m.connect(c, "out", add, "a"));        // Float Out -> Float In: ok
        CHECK(m.graph().incoming(add, "a").has_value());
        CHECK(!m.connect(entry, "then", add, "b"));  // Exec -> Float: rejected
        CHECK(!m.graph().incoming(add, "b").has_value());
        CHECK(!m.connect(c, "out", add, "result"));  // target is an Out pin: rejected

        m.deleteNode(c);
        CHECK(m.graph().node(c) == nullptr);
        CHECK(!m.graph().incoming(add, "a").has_value());  // wire removed with node
    }

    // data input cardinality: a second source replaces the first.
    {
        GraphEditorModel m(&reg);
        const NodeId c1 = m.addNode("Const", 0, 0);
        const NodeId c2 = m.addNode("Const", 0, 0);
        const NodeId add = m.addNode("Add", 0, 0);
        CHECK(m.connect(c1, "out", add, "a"));
        CHECK(m.connect(c2, "out", add, "a"));   // replaces
        const auto inc = m.graph().incoming(add, "a");
        CHECK(inc.has_value());
        CHECK(inc->fromNode == c2);
    }

    // run() executes the graph and fills outputs.
    {
        GraphEditorModel m(&reg);
        const NodeId entry = m.addNode("Entry", 0, 0);
        const NodeId set = m.addNode("SetOutput", 0, 0);
        m.setLiteral(set, "key", NodeValue::S("r"));
        m.setLiteral(set, "value", NodeValue::F(7.0f));
        CHECK(m.connect(entry, "then", set, "in"));
        m.run();
        CHECK_NEAR(m.lastRun().outputs.at("r").asFloat(), 7.0f);
    }

    // JSON round-trip via the model; loadFromJson clears dirty; malformed -> false.
    {
        GraphEditorModel m(&reg);
        const NodeId entry = m.addNode("Entry", 0, 0);
        const NodeId set = m.addNode("SetOutput", 0, 0);
        m.setLiteral(set, "key", NodeValue::S("v"));
        m.setLiteral(set, "value", NodeValue::F(5.0f));
        m.connect(entry, "then", set, "in");
        const nlohmann::json j = m.toJson();

        GraphEditorModel m2(&reg);
        CHECK(m2.loadFromJson(j));
        CHECK(!m2.dirty());
        m2.run();
        CHECK_NEAR(m2.lastRun().outputs.at("v").asFloat(), 5.0f);

        CHECK(!m2.loadFromJson(nlohmann::json::parse(R"({"nodes":[{"id":1,"type":"Bogus"}]})")));
    }

    // exec output cardinality: a second target replaces the first.
    {
        GraphEditorModel m(&reg);
        const NodeId entry = m.addNode("Entry", 0, 0);
        const NodeId s1 = m.addNode("SetOutput", 0, 0);
        const NodeId s2 = m.addNode("SetOutput", 0, 0);
        CHECK(m.connect(entry, "then", s1, "in"));
        CHECK(m.connect(entry, "then", s2, "in"));   // replaces
        CHECK(!m.graph().incoming(s1, "in").has_value());
        CHECK(m.graph().incoming(s2, "in").has_value());
        CHECK(m.graph().outgoing(entry, "then")->toNode == s2);
    }

    // direct disconnect on the model removes the wire + dirties.
    {
        GraphEditorModel m(&reg);
        const NodeId entry = m.addNode("Entry", 0, 0);
        const NodeId set = m.addNode("SetOutput", 0, 0);
        CHECK(m.connect(entry, "then", set, "in"));
        m.clearDirty();
        m.disconnect(set, "in");
        CHECK(!m.graph().incoming(set, "in").has_value());
        CHECK(m.dirty());
    }

    // int<->float compatibility via a local registry with an Int input port.
    {
        NodeRegistry localReg;
        localReg.registerType({"IntSink", "Test",
            { PortDesc{"in", PortType::Int, PortDir::In} },
            [](NodeContext&) {} });
        localReg.registerType({"FloatSrc", "Test",
            { PortDesc{"out", PortType::Float, PortDir::Out} },
            [](NodeContext&) {} });
        GraphEditorModel m(&localReg);
        const NodeId src = m.addNode("FloatSrc", 0, 0);
        const NodeId snk = m.addNode("IntSink", 0, 0);
        CHECK(m.connect(src, "out", snk, "in"));   // Float Out -> Int In: accepted
    }

    return iron_test_result();
}
