#include "editor/NodeGraphPanel.h"

#include "nodes/GraphEditorModel.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"

#include <imgui.h>
#include <imgui_node_editor.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

namespace ed = ax::NodeEditor;

namespace iron {

namespace {

std::uintptr_t pinId(NodeId node, int portIndex, bool isOutput) {
    return (static_cast<std::uintptr_t>(node) << 8) |
           (static_cast<std::uintptr_t>(portIndex) << 1) |
           (isOutput ? 1u : 0u);
}
void decodePin(std::uintptr_t id, NodeId& node, int& portIndex, bool& isOutput) {
    isOutput  = (id & 1u) != 0;
    portIndex = static_cast<int>((id >> 1) & 0x7F);
    node      = static_cast<NodeId>(id >> 8);
}
const PortDesc* resolvePin(const GraphEditorModel& m, std::uintptr_t id,
                           NodeId& node, std::string& portName) {
    int idx; bool out;
    decodePin(id, node, idx, out);
    const Node* n = m.graph().node(node);
    if (!n || !m.registry()) return nullptr;
    const NodeTypeDesc* t = m.registry()->find(n->typeName);
    if (!t || idx < 0 || idx >= static_cast<int>(t->ports.size())) return nullptr;
    portName = t->ports[idx].name;
    return &t->ports[idx];
}
int findPortIndex(const GraphEditorModel& m, NodeId node, const std::string& port, bool wantOut) {
    const Node* n = m.graph().node(node);
    if (!n || !m.registry()) return -1;
    const NodeTypeDesc* t = m.registry()->find(n->typeName);
    if (!t) return -1;
    for (int i = 0; i < static_cast<int>(t->ports.size()); ++i)
        if (t->ports[i].name == port && (t->ports[i].dir == PortDir::Out) == wantOut) return i;
    return -1;
}

}  // namespace

NodeGraphPanel::NodeGraphPanel() { ctx_ = ed::CreateEditor(); }
NodeGraphPanel::~NodeGraphPanel() { if (ctx_) ed::DestroyEditor(ctx_); }

bool NodeGraphPanel::draw(GraphEditorModel& model) {
    ImGui::Begin("Node Editor");

    if (ImGui::Button("Run")) model.run();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("##path", savePath_, sizeof(savePath_));
    ImGui::SameLine();
    if (ImGui::Button("Save")) { std::ofstream f(savePath_); if (f) f << model.toJson().dump(2); }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        std::ifstream f(savePath_);
        if (f) {
            try {
                nlohmann::json j; f >> j;
                if (model.loadFromJson(j))
                    placed_.clear();   // re-place restored nodes from their saved positions
            } catch (const std::exception&) {
                // malformed file: ignore, leave graph unchanged
            }
        }
    }
    // M55: assign the current graph to the selected entity (host handles the wiring).
    bool assignClicked = false;
    ImGui::SameLine();
    if (ImGui::Button("Assign to entity")) assignClicked = true;
    // Palette
    if (model.registry()) {
        ImGui::TextUnformatted("Add:");
        for (const NodeTypeDesc* t : model.registry()->all()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(t->typeName.c_str())) {
                model.addNode(t->typeName, spawnX_, spawnY_);
                spawnX_ += 30.0f; spawnY_ += 30.0f;
                if (spawnX_ > 400.0f) { spawnX_ = 40.0f; spawnY_ = 40.0f; }
            }
        }
    }
    // Outputs readout (the visible result of Run).
    ImGui::Separator();
    ImGui::TextUnformatted("Run outputs:");
    if (model.lastRun().outputs.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(none yet - wire Entry -> ... -> SetOutput, then Run)");
    } else {
        for (const auto& kv : model.lastRun().outputs) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "%s = %.3f",
                               kv.first.c_str(), kv.second.asFloat());
        }
    }
    ImGui::Separator();

    ed::SetCurrentEditor(ctx_);
    ed::Begin("canvas");

    for (const Node& n : model.graph().nodes()) {
        const NodeTypeDesc* t = model.registry() ? model.registry()->find(n.typeName) : nullptr;
        if (!t) continue;
        if (placed_.find(n.id) == placed_.end()) {
            ed::SetNodePosition(ed::NodeId(static_cast<std::uintptr_t>(n.id)),
                                ImVec2(n.editorX, n.editorY));
            placed_.insert(n.id);
        }
        ed::BeginNode(ed::NodeId(static_cast<std::uintptr_t>(n.id)));
        ImGui::TextUnformatted(n.typeName.c_str());
        for (int i = 0; i < static_cast<int>(t->ports.size()); ++i) {
            const PortDesc& p = t->ports[i];
            const bool isOut = p.dir == PortDir::Out;
            ed::BeginPin(ed::PinId(pinId(n.id, i, isOut)),
                         isOut ? ed::PinKind::Output : ed::PinKind::Input);
            ImGui::Text("%s%s", isOut ? "" : "-> ", p.name.c_str());
            ed::EndPin();
            if (!isOut && p.type != PortType::Exec &&
                !model.graph().incoming(n.id, p.name).has_value()) {
                ImGui::SameLine();
                const Node* nn = model.graph().node(n.id);
                auto it = nn->literals.find(p.name);
                const std::string wid = "##" + p.name + std::to_string(n.id);
                if (p.type == PortType::Float || p.type == PortType::Int) {
                    float val = (it != nn->literals.end()) ? it->second.asFloat() : 0.0f;
                    ImGui::SetNextItemWidth(70);
                    if (ImGui::DragFloat(wid.c_str(), &val, 0.1f))
                        model.setLiteral(n.id, p.name, NodeValue::F(val));
                } else if (p.type == PortType::Bool) {
                    bool val = (it != nn->literals.end()) ? it->second.asBool() : false;
                    if (ImGui::Checkbox(wid.c_str(), &val))
                        model.setLiteral(n.id, p.name, NodeValue::B(val));
                } else if (p.type == PortType::String) {
                    char buf[128];
                    std::string s = (it != nn->literals.end()) ? it->second.asString() : "";
                    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
                    ImGui::SetNextItemWidth(90);
                    if (ImGui::InputText(wid.c_str(), buf, sizeof(buf)))
                        model.setLiteral(n.id, p.name, NodeValue::S(buf));
                }
            }
        }
        ed::EndNode();
    }

    const auto& conns = model.graph().connections();
    for (std::size_t i = 0; i < conns.size(); ++i) {
        const Connection& c = conns[i];
        const int fi = findPortIndex(model, c.fromNode, c.fromPort, true);
        const int ti = findPortIndex(model, c.toNode, c.toPort, false);
        if (fi < 0 || ti < 0) continue;
        ed::Link(ed::LinkId(static_cast<std::uintptr_t>(i + 1)),
                 ed::PinId(pinId(c.fromNode, fi, true)),
                 ed::PinId(pinId(c.toNode, ti, false)));
    }

    if (ed::BeginCreate()) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b)) {
            if (a && b && ed::AcceptNewItem()) {
                NodeId an, bn; std::string ap, bp;
                const PortDesc* pa = resolvePin(model, a.Get(), an, ap);
                const PortDesc* pb = resolvePin(model, b.Get(), bn, bp);
                if (pa && pb) {
                    if (pa->dir == PortDir::Out && pb->dir == PortDir::In) model.connect(an, ap, bn, bp);
                    else if (pb->dir == PortDir::Out && pa->dir == PortDir::In) model.connect(bn, bp, an, ap);
                }
            }
        }
    }
    ed::EndCreate();

    if (ed::BeginDelete()) {
        // Snapshot connections BEFORE any disconnect: model.disconnect mutates
        // the graph's vector, which would otherwise shift the link-id->index
        // mapping for later links deleted in this same block (e.g. deleting a
        // node with multiple incoming wires).
        const std::vector<Connection> connSnapshot = model.graph().connections();
        ed::LinkId lid;
        while (ed::QueryDeletedLink(&lid)) {
            if (ed::AcceptDeletedItem()) {
                const std::size_t idx = static_cast<std::size_t>(lid.Get()) - 1;
                if (idx < connSnapshot.size())
                    model.disconnect(connSnapshot[idx].toNode, connSnapshot[idx].toPort);
            }
        }
        ed::NodeId nid;
        while (ed::QueryDeletedNode(&nid)) {
            if (ed::AcceptDeletedItem()) model.deleteNode(static_cast<NodeId>(nid.Get()));
        }
    }
    ed::EndDelete();

    ed::End();
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
    return assignClicked;
}

}  // namespace iron
