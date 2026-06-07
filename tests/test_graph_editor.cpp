#include "nodes/GraphEditorModel.h"
#include "nodes/NodeRegistry.h"
#include "nodes/BuiltinNodes.h"
#include "nodes/NodeContext.h"
#include "nodes/NodeGraphIO.h"   // iron::fromJson (runtime-tolerance check)
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

    // setNodePosition updates editorX/Y + dirties; positions round-trip JSON.
    {
        GraphEditorModel m(&reg);
        const NodeId n = m.addNode("Const", 0, 0);
        m.clearDirty();
        m.setNodePosition(n, 123.0f, 45.0f);
        CHECK(m.dirty());
        CHECK_NEAR(m.graph().node(n)->editorX, 123.0f);
        CHECK_NEAR(m.graph().node(n)->editorY, 45.0f);

        const nlohmann::json j = m.toJson();
        GraphEditorModel m2(&reg);
        CHECK(m2.loadFromJson(j));
        CHECK_NEAR(m2.graph().node(n)->editorX, 123.0f);
        CHECK_NEAR(m2.graph().node(n)->editorY, 45.0f);
    }

    // M58: comments — add / mutate / delete + dirty + JSON round-trip.
    {
        GraphEditorModel m(&reg);
        const std::uint32_t id = m.addComment(10, 20, 200, 100, "Update UI");
        CHECK(m.comments().size() == 1u);
        CHECK(m.comments()[0].id == id);
        CHECK_NEAR(m.comments()[0].x, 10.0f);
        CHECK(m.comments()[0].title == "Update UI");
        CHECK(m.dirty());

        m.clearDirty();
        m.setCommentRect(id, 11, 22, 210, 110);
        CHECK(m.dirty());
        CHECK_NEAR(m.comments()[0].w, 210.0f);

        m.clearDirty();
        m.setCommentTitle(id, "Renamed");
        CHECK(m.dirty());
        CHECK(m.comments()[0].title == "Renamed");

        // no-op edits must NOT re-dirty (protects M57 undo coalescing).
        m.clearDirty();
        m.setCommentTitle(id, "Renamed");           // same value
        CHECK(!m.dirty());
        m.setCommentRect(id, 11, 22, 210, 110);     // same value as last set
        CHECK(!m.dirty());

        GraphEditorModel m2(&reg);
        CHECK(m2.loadFromJson(m.toJson()));
        CHECK(m2.comments().size() == 1u);
        CHECK(m2.comments()[0].id == id);
        CHECK(m2.comments()[0].title == "Renamed");
        CHECK_NEAR(m2.comments()[0].h, 110.0f);

        m.clearDirty();
        m.deleteComment(id);
        CHECK(m.comments().empty());
        CHECK(m.dirty());
    }

    // M58: multiple comments — delete isolation + id stability after load+add.
    {
        GraphEditorModel m(&reg);
        const std::uint32_t a = m.addComment(0, 0, 100, 100, "A");
        const std::uint32_t b = m.addComment(50, 50, 100, 100, "B");
        CHECK(a != b);
        m.deleteComment(a);
        CHECK(m.comments().size() == 1u);
        CHECK(m.comments()[0].id == b);          // only A removed, B intact
        CHECK(m.comments()[0].title == "B");

        // After a round-trip, a fresh addComment must not collide with loaded ids.
        GraphEditorModel m2(&reg);
        CHECK(m2.loadFromJson(m.toJson()));
        const std::uint32_t c = m2.addComment(10, 10, 80, 80, "C");
        CHECK(c != b);                           // nextCommentId_ restored past b
    }

    // M58: runtime tolerance — the executable loader ignores "comments".
    {
        GraphEditorModel m(&reg);
        m.addNode("Entry", 0, 0);
        m.addComment(0, 0, 100, 100, "note");
        const nlohmann::json j = m.toJson();
        CHECK(j.contains("comments"));
        auto g = iron::fromJson(j, reg);   // the Play-mode path
        CHECK(g.has_value());              // comments don't break executable load
    }

    // M59: portsCompatible parity (pure, no registry).
    {
        const PortDesc outF{"o", PortType::Float, PortDir::Out};
        const PortDesc inF {"i", PortType::Float, PortDir::In};
        const PortDesc inI {"i", PortType::Int,   PortDir::In};
        const PortDesc outE{"o", PortType::Exec,  PortDir::Out};
        const PortDesc inE {"i", PortType::Exec,  PortDir::In};
        CHECK(portsCompatible(outF, inF));    // float->float
        CHECK(portsCompatible(outF, inI));    // float->int
        CHECK(portsCompatible(outE, inE));    // exec->exec
        CHECK(!portsCompatible(outF, inE));   // data->exec mismatch
        CHECK(!portsCompatible(outE, inF));   // exec->data mismatch
        CHECK(!portsCompatible(outF, outF));  // Out->Out
        CHECK(!portsCompatible(inF, inF));    // In as source
    }

    // M59: compatibleCreations — every offered creation is genuinely connectable.
    {
        GraphEditorModel m(&reg);
        const NodeId src = m.addNode("Const", 0, 0);   // Const has a Float "out"
        const auto fromFloat = m.compatibleCreations(PortType::Float, PortDir::Out);
        CHECK(!fromFloat.empty());
        for (const auto& c : fromFloat) {
            const NodeId n = m.addNode(c.typeName, 0, 0);
            CHECK(m.connect(src, "out", n, c.targetPort));   // must actually connect
            m.deleteNode(n);
        }
        const auto fromExec = m.compatibleCreations(PortType::Exec, PortDir::Out);
        bool hasBranch = false, hasConst = false;
        for (const auto& c : fromExec) {
            if (c.typeName == "Branch") hasBranch = true;
            if (c.typeName == "Const")  hasConst  = true;
        }
        CHECK(hasBranch);
        CHECK(!hasConst);

        // In-source direction (dragging from an input pin): offered types must
        // have an OUTPUT that can feed a Float input. Verify each connects into
        // an Add node's Float input "a".
        const auto fromFloatIn = m.compatibleCreations(PortType::Float, PortDir::In);
        CHECK(!fromFloatIn.empty());
        bool inHasConst = false;
        const NodeId sink = m.addNode("Add", 0, 0);   // Add has Float In "a"
        for (const auto& cr : fromFloatIn) {
            if (cr.typeName == "Const") inHasConst = true;
            const NodeId n = m.addNode(cr.typeName, 0, 0);
            CHECK(m.connect(n, cr.targetPort, sink, "a"));   // offered Out -> Float In
            m.deleteNode(n);
        }
        CHECK(inHasConst);
    }

    // M61: catalogToJson emits subtitle + devOnly per type.
    {
        const nlohmann::json cat = catalogToJson(reg);
        CHECK(cat.is_array());
        bool sawSubtitle = false, sawDevOnly = false;
        for (const auto& e : cat) {
            CHECK(e.contains("subtitle"));
            CHECK(e.contains("devOnly"));
            if (e.value("typeName", std::string()) == "Branch" && !e.value("subtitle", std::string()).empty())
                sawSubtitle = true;
            if (e.value("devOnly", false)) sawDevOnly = true;
        }
        CHECK(sawSubtitle);   // Branch gets an authored subtitle
        CHECK(sawDevOnly);    // at least one node is dev-only
    }

    // M61: buildCreateList grouping + search + compat filter.
    {
        auto groups = buildCreateList(reg, "", nullptr);
        std::size_t total = 0; bool sawFlow = false;
        for (const auto& g : groups) { total += g.types.size(); if (g.category == "Flow") sawFlow = true; }
        CHECK(sawFlow);
        CHECK(total == reg.all().size());     // no type dropped

        auto hits = buildCreateList(reg, "BRANCH", nullptr);  // case-insensitive
        std::size_t hitCount = 0; bool sawBranch = false;
        for (const auto& g : hits) for (const NodeTypeDesc* t : g.types) {
            ++hitCount; if (t->typeName == "Branch") sawBranch = true;
        }
        CHECK(sawBranch);
        CHECK(hitCount < reg.all().size());   // filtered

        const std::vector<std::string> allowed = {"Add", "Compare"};
        auto restricted = buildCreateList(reg, "", &allowed);
        std::size_t rCount = 0;
        for (const auto& g : restricted) for (const NodeTypeDesc* t : g.types) {
            ++rCount; CHECK(t->typeName == "Add" || t->typeName == "Compare");
        }
        CHECK(rCount == 2u);
    }

    // M61: unsaved() is independent of the transient dirty() signal — the undo
    // system clears dirty() every frame, which must NOT clear the unsaved state.
    {
        GraphEditorModel m(&reg);
        CHECK(!m.unsaved());                 // fresh model is clean
        const NodeId id = m.addNode("Const", 0, 0);
        CHECK(m.dirty());
        CHECK(m.unsaved());                  // a mutation sets both
        m.clearDirty();                      // undo consumes the per-frame signal
        CHECK(!m.dirty());
        CHECK(m.unsaved());                  // ...but unsaved persists (the bug we fixed)
        m.markSaved();                       // a Save/Load clears it
        CHECK(!m.unsaved());
        m.setNodePosition(id, 5, 5);         // any later edit re-flags unsaved
        CHECK(m.unsaved());
    }

    return iron_test_result();
}
