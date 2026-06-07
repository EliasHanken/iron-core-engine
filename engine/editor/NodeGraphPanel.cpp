#include "editor/NodeGraphPanel.h"

#include "nodes/GraphEditorModel.h"
#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"

#include <imgui.h>
#include <imgui_node_editor.h>
#include "utilities/widgets.h"
#include "IconsForkAwesome.h"   // M61: node category glyphs

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

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

ImColor pinColor(PortType t) {
    switch (t) {
        case PortType::Exec:   return ImColor(255, 255, 255);
        case PortType::Bool:   return ImColor(220,  48,  48);
        case PortType::Int:    return ImColor( 68, 201, 156);
        case PortType::Float:  return ImColor(147, 226,  74);
        case PortType::Vec2:
        case PortType::Vec3:
        case PortType::Vec4:   return ImColor(245, 201,   0);
        case PortType::String: return ImColor(218,  60, 156);
    }
    return ImColor(255, 255, 255);
}

ImColor headerColor(const std::string& category) {
    if (category == "Event")     return ImColor(150,  45,  45);
    if (category == "Flow")      return ImColor( 60,  60,  90);
    if (category == "Math")      return ImColor( 45, 120,  60);
    if (category == "Transform") return ImColor(135,  90,  30);
    if (category == "Variable")  return ImColor(105,  45, 135);
    if (category == "Value")     return ImColor( 30, 105, 120);
    if (category == "Sink")      return ImColor( 90,  90,  90);
    return ImColor( 75,  75,  75);
}

// M61: Fork Awesome glyph per node category, for the header + create menu.
const char* nodeCategoryIcon(const std::string& category) {
    if (category == "Event")     return ICON_FK_BOLT;
    if (category == "Flow")      return ICON_FK_SITEMAP;
    if (category == "Math")      return ICON_FK_CALCULATOR;
    if (category == "Transform") return ICON_FK_ARROWS;
    if (category == "Variable")  return ICON_FK_CUBE;
    if (category == "Value")     return ICON_FK_HASHTAG;
    if (category == "Sink")      return ICON_FK_SIGN_OUT;
    return ICON_FK_SQUARE_O;
}

ax::Widgets::IconType iconFor(PortType t) {
    return t == PortType::Exec ? ax::Widgets::IconType::Flow
                               : ax::Widgets::IconType::Circle;
}

// Renders the search box + grouped create list; returns the chosen type name
// ("" = nothing chosen this frame). searchBuf persists across frames (a panel member).
std::string drawCreateList(GraphEditorModel& model, char* searchBuf, std::size_t searchCap,
                           const std::vector<std::string>* allowedTypeNames) {
    std::string chosen;
    if (!model.registry()) return chosen;
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##create_search", "Search...", searchBuf, searchCap);
    ImGui::Separator();
    const auto groups = buildCreateList(*model.registry(), searchBuf, allowedTypeNames);
    if (groups.empty()) { ImGui::TextDisabled("(no matches)"); return chosen; }
    const bool searching = searchBuf[0] != '\0';
    for (const NodeCreateGroup& g : groups) {
        if (!ImGui::CollapsingHeader(g.category.c_str(),
                searching ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None))
            continue;
        for (const NodeTypeDesc* t : g.types) {
            ImGui::PushID(t->typeName.c_str());
            const std::string label = std::string(nodeCategoryIcon(t->category)) + "  " + t->typeName;
            // Subtitle goes in MenuItem's shortcut slot: right-aligned + dimmed +
            // part of the selectable row (so it tracks the hover highlight and
            // clips within the popup, unlike a SameLine TextDisabled).
            if (ImGui::MenuItem(label.c_str(),
                                t->subtitle.empty() ? nullptr : t->subtitle.c_str()))
                chosen = t->typeName;
            ImGui::PopID();
        }
    }
    return chosen;
}

}  // namespace

NodeGraphPanel::NodeGraphPanel() {
    // M61 fix: disable the node-editor's OWN settings file ("NodeEditor.json").
    // Otherwise layout has two competing sources of truth — our model (saved to
    // node_graph.json) and the editor's internal store — which fight over comment
    // position/size on reload (the editor's restored m_GroupBounds wins, so saved
    // comment sizes never re-apply). Our model is the single source of truth.
    ed::Config cfg;
    cfg.SettingsFile = nullptr;
    ctx_ = ed::CreateEditor(&cfg);
    // M58: soften the node cards to match the blueprint example.
    ed::SetCurrentEditor(ctx_);
    ed::Style& st = ed::GetStyle();
    st.NodeRounding    = 8.0f;
    st.NodeBorderWidth = 1.5f;
    // UE5 relationship: a MEDIUM-dark canvas with NODES that are DARKER + slightly
    // transparent — the grid faintly shows through, so cards read as recessed dark
    // panels rather than light cards.
    st.Colors[ed::StyleColor_Bg]         = ImColor(30, 30, 35, 255);   // medium-dark canvas
    st.Colors[ed::StyleColor_Grid]       = ImColor(255, 255, 255, 14); // subtle grid
    st.Colors[ed::StyleColor_NodeBg]     = ImColor(14, 14, 16, 232);   // near-black, ~91% opaque (darker than canvas)
    st.Colors[ed::StyleColor_NodeBorder] = ImColor(255, 255, 255, 55); // subtle light outline
    ed::SetCurrentEditor(nullptr);
}
NodeGraphPanel::~NodeGraphPanel() { if (ctx_) ed::DestroyEditor(ctx_); }

NodeGraphPanel::Action NodeGraphPanel::draw(GraphEditorModel& model, const char* targetName, bool targetHasGraph) {
    if (!ImGui::Begin("Node Editor")) {
        // Collapsed or an inactive dock tab: ImGui sets SkipItems, so ed::Group's
        // Dummy advances 0 and collapses comment group bounds — and the read-back
        // below would then write that collapsed size back into the model. Skip all
        // rendering while hidden (End() is still required to balance Begin()).
        focused_ = false;
        ImGui::End();
        return Action::None;
    }
    focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (ImGui::Button("Run")) model.run();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("##path", savePath_, sizeof(savePath_));
    ImGui::SameLine();
    // M61: unsaved indicator — a "*" on Save plus an amber tag so the user never
    // leaves with unwritten edits. unsaved() is set by every model mutation and
    // cleared on a successful Save/Load. (Uses unsaved(), NOT dirty(): the host's
    // undo system consumes and clears dirty() every frame.)
    const bool unsaved = model.unsaved();
    if (ImGui::Button(unsaved ? "Save*" : "Save")) {
        std::ofstream f(savePath_);
        if (f) { f << model.toJson().dump(2); model.markSaved(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        std::ifstream f(savePath_);
        if (f) {
            try {
                nlohmann::json j; f >> j;
                if (model.loadFromJson(j)) {
                    resetPlacement();   // re-place restored nodes + comments from their saved positions
                    model.markSaved();  // freshly loaded == matches the file
                }
            } catch (const std::exception&) {
                // malformed file: ignore, leave graph unchanged
            }
        }
    }
    if (unsaved) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.20f, 1.0f), ICON_FK_EXCLAMATION_CIRCLE " Unsaved");
    }
    // M55: entity-aware target readout + load/assign (host handles the wiring).
    Action action = Action::None;
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (targetName) ImGui::Text("Entity: %s", targetName);
    else            ImGui::TextDisabled("Entity: (none selected)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!targetName || !targetHasGraph);
    if (ImGui::Button("Load from entity")) action = Action::LoadFromEntity;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!targetName);
    if (ImGui::Button("Assign to entity")) action = Action::Assign;
    ImGui::EndDisabled();
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

    // M58: comment editor-ids live in a high-bit namespace so they never collide
    // with node ids, pin ids (node<<8|...), or link ids (i+1).
    auto commentEd = [](std::uint32_t id) -> std::uintptr_t {
        return (std::uintptr_t{1} << 62) | id;
    };

    for (const Comment& c : model.comments()) {
        const ed::NodeId cid(commentEd(c.id));
        if (placedComments_.find(c.id) == placedComments_.end()) {
            ed::SetNodePosition(cid, ImVec2(c.x, c.y));
            // ed::Group(size) only adopts its size arg the first frame a node
            // becomes a group; afterwards the editor's stored m_GroupBounds wins.
            // So push the model's size in explicitly on every (re)placement —
            // this is what makes a Load/reopen restore the saved comment size.
            ed::SetGroupSize(cid, ImVec2(c.w, c.h));
            placedComments_.insert(c.id);
        }
        ed::PushStyleColor(ed::StyleColor_NodeBg,     ImColor(60, 60, 75, 80).Value);
        ed::PushStyleColor(ed::StyleColor_NodeBorder, ImColor(130, 130, 160, 180).Value);
        ed::BeginNode(cid);
        ImGui::PushID(static_cast<int>(c.id) ^ 0x4000);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", c.title.c_str());
        ImGui::SetNextItemWidth(c.w > 48.0f ? c.w - 16.0f : 96.0f);
        if (ImGui::InputText("##ctitle", buf, sizeof(buf)))
            model.setCommentTitle(c.id, buf);
        ed::Group(ImVec2(c.w, c.h));
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleColor(2);
    }

    for (const Node& n : model.graph().nodes()) {
        const NodeTypeDesc* t = model.registry() ? model.registry()->find(n.typeName) : nullptr;
        if (!t) continue;
        if (placed_.find(n.id) == placed_.end()) {
            ed::SetNodePosition(ed::NodeId(static_cast<std::uintptr_t>(n.id)),
                                ImVec2(n.editorX, n.editorY));
            placed_.insert(n.id);
        }
        ed::BeginNode(ed::NodeId(static_cast<std::uintptr_t>(n.id)));
        ImGui::PushID(static_cast<int>(n.id));
        ImGui::BeginGroup();   // M58: wrap content for reliable screen-space bounds

        // Header: category icon + white title, then optional dim subtitle.
        // The colored band is drawn behind it after EndNode.
        ImGui::TextColored(ImVec4(0.78f, 0.84f, 0.92f, 1.0f), "%s", nodeCategoryIcon(t->category));
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", t->typeName.c_str());
        float headerBottom = ImGui::GetItemRectMax().y;
        if (!t->subtitle.empty()) {
            ImGui::TextColored(ImVec4(0.62f, 0.64f, 0.70f, 1.0f), "%s", t->subtitle.c_str());
            headerBottom = ImGui::GetItemRectMax().y;
        }
        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        const float iconSz = 16.0f;

        // Left column: inputs (icon + label + inline literal for unconnected data).
        ImGui::BeginGroup();
        for (int i = 0; i < static_cast<int>(t->ports.size()); ++i) {
            const PortDesc& p = t->ports[i];
            if (p.dir != PortDir::In) continue;
            const bool connected = model.graph().incoming(n.id, p.name).has_value();
            ed::BeginPin(ed::PinId(pinId(n.id, i, false)), ed::PinKind::Input);
            ax::Widgets::Icon(ImVec2(iconSz, iconSz), iconFor(p.type), connected,
                              pinColor(p.type).Value, ImColor(32, 32, 32, 255).Value);
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextUnformatted(p.name.c_str());
            ed::EndPin();
            if (p.type != PortType::Exec && !connected) {
                ImGui::SameLine();
                const Node* nn = model.graph().node(n.id);
                auto it = nn->literals.find(p.name);
                const std::string wid = "##" + p.name + std::to_string(n.id);
                if (p.type == PortType::Float || p.type == PortType::Int) {
                    float val = (it != nn->literals.end()) ? it->second.asFloat() : 0.0f;
                    ImGui::SetNextItemWidth(60);
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
                    ImGui::SetNextItemWidth(80);
                    if (ImGui::InputText(wid.c_str(), buf, sizeof(buf)))
                        model.setLiteral(n.id, p.name, NodeValue::S(buf));
                }
            }
        }
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 24.0f);   // column gap

        // Right column: outputs (label + icon).
        ImGui::BeginGroup();
        for (int i = 0; i < static_cast<int>(t->ports.size()); ++i) {
            const PortDesc& p = t->ports[i];
            if (p.dir != PortDir::Out) continue;
            const bool outConnected = model.graph().outgoing(n.id, p.name).has_value();
            ed::BeginPin(ed::PinId(pinId(n.id, i, true)), ed::PinKind::Output);
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::SameLine(0.0f, 4.0f);
            ax::Widgets::Icon(ImVec2(iconSz, iconSz), iconFor(p.type), outConnected,
                              pinColor(p.type).Value, ImColor(32, 32, 32, 255).Value);
            ed::EndPin();
        }
        ImGui::EndGroup();

        ImGui::EndGroup();   // M58: close the content group
        const ImVec2 contentMin = ImGui::GetItemRectMin();
        const ImVec2 contentMax = ImGui::GetItemRectMax();
        ImGui::PopID();
        ed::EndNode();

        // M58/M61: node decorations drawn into the node background list. Positions
        // use the content's SCREEN rect (contentMin/Max ± NodePadding) — the same
        // space the bg list draws in. Clipped to that card rect; rounded fills use
        // RoundCorners flags and the glass sheen lives in the upper body only
        // (straight side edges, fades out before the rounded bottom corners).
        if (ImDrawList* bg = ed::GetNodeBackgroundDrawList(
                ed::NodeId(static_cast<std::uintptr_t>(n.id)))) {
            const ImVec4 pad      = ed::GetStyle().NodePadding;   // x=left y=top z=right w=bottom
            const float  rounding = ed::GetStyle().NodeRounding;
            const ImVec2 cardMin(contentMin.x - pad.x, contentMin.y - pad.y);
            const ImVec2 cardMax(contentMax.x + pad.z, contentMax.y + pad.w);
            bg->PushClipRect(cardMin, cardMax, true);

            // Header band (rounded top): category color, translucent gloss-ramp
            // gradient, dark contrast overlay, separator under the subtitle.
            const ImVec2 a = cardMin;
            const ImVec2 b(cardMax.x, headerBottom + 3.0f);
            // The blueprint header texture IS the header, multiplied by the
            // category color (its built-in gradient + sheen give the non-flat
            // look). Falls back to a plain colored band if the texture is absent.
            if (headerTex_)
                bg->AddImageRounded(reinterpret_cast<ImTextureID>(headerTex_), a, b,
                                    ImVec2(0, 0), ImVec2(1, 1), headerColor(t->category),
                                    rounding, ImDrawFlags_RoundCornersTop);
            else
                bg->AddRectFilled(a, b, headerColor(t->category), rounding, ImDrawFlags_RoundCornersTop);
            bg->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), IM_COL32(0, 0, 0, 120), 1.0f);

            // M61: glass — a soft sheen confined to the UPPER BODY (just under the
            // header), drawn full-width so it aligns with the body's straight side
            // edges, and fading to zero alpha before the rounded bottom corners.
            // This keeps the glossy glass falloff WITHOUT the inset "light grey box"
            // / boxy-corner artifacts of a full-card gradient (AddRectFilledMultiColor
            // can't round corners, so a full-card fill either insets into a visible
            // box or overhangs the rounded corners).
            const float  bodyTop = b.y;
            const ImVec2 sheenMin(cardMin.x, bodyTop);
            const ImVec2 sheenMax(cardMax.x, bodyTop + (cardMax.y - bodyTop) * 0.55f);
            bg->AddRectFilledMultiColor(sheenMin, sheenMax,
                                        IM_COL32(255, 255, 255, 26), IM_COL32(255, 255, 255, 26),
                                        IM_COL32(255, 255, 255, 0),  IM_COL32(255, 255, 255, 0));

            // M61: dev-only striped tag along the card's bottom edge.
            if (t->devOnly) {
                const float stripeH = 6.0f;
                const ImVec2 sMin(cardMin.x, cardMax.y - stripeH);
                const ImVec2 sMax(cardMax.x, cardMax.y);
                bg->AddRectFilled(sMin, sMax, IM_COL32(20, 20, 20, 255), rounding, ImDrawFlags_RoundCornersBottom);
                const float step = 10.0f;
                for (float x = sMin.x - stripeH; x < sMax.x; x += step)
                    bg->AddQuadFilled(ImVec2(x, sMax.y), ImVec2(x + stripeH, sMin.y),
                                      ImVec2(x + stripeH + 4.0f, sMin.y), ImVec2(x + 4.0f, sMax.y),
                                      IM_COL32(225, 190, 0, 200));
            }
            bg->PopClipRect();
        }
    }

    // M56: sync live canvas positions back into the model so drags persist
    // through Save/Assign (loadFromJson + placed_ restore them). Only write on a
    // real change (>0.5px) — the editor floors positions, so an exact compare
    // would re-dirty a freshly loaded graph from sub-pixel drift, making the
    // unsaved indicator lie. Mirrors setCommentRect's tolerance.
    for (const Node& n : model.graph().nodes()) {
        const ImVec2 pos = ed::GetNodePosition(ed::NodeId(static_cast<std::uintptr_t>(n.id)));
        if (std::fabs(pos.x - n.editorX) > 0.5f || std::fabs(pos.y - n.editorY) > 0.5f)
            model.setNodePosition(n.id, pos.x, pos.y);
    }

    // M58: persist comment move/resize. GetNodeSize includes the title row +
    // padding, so subtract a once-measured offset to recover the group size.
    for (const Comment& c : model.comments()) {
        const ed::NodeId cid(commentEd(c.id));
        const ImVec2 pos = ed::GetNodePosition(cid);
        const ImVec2 sz  = ed::GetNodeSize(cid);
        if (commentOffX_.find(c.id) == commentOffX_.end()) {
            commentOffX_[c.id] = sz.x - c.w;   // node-size overhead vs group size
            commentOffY_[c.id] = sz.y - c.h;
        }
        const float w = sz.x - commentOffX_[c.id];
        const float h = sz.y - commentOffY_[c.id];
        if (pos.x != c.x || pos.y != c.y || w != c.w || h != c.h)
            model.setCommentRect(c.id, pos.x, pos.y,
                                 w > 32.0f ? w : 32.0f, h > 32.0f ? h : 32.0f);
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
        // M59: drag from a pin onto empty canvas -> open a type-compatible
        // create menu and auto-wire the new node to the dragged pin.
        ed::PinId newNodePin = 0;
        if (ed::QueryNewNode(&newNodePin)) {
            if (ed::AcceptNewItem()) {
                const ImVec2 canvasPos = ed::ScreenToCanvas(ImGui::GetMousePos());
                pendingCreatePin_ = static_cast<unsigned long long>(newNodePin.Get());
                pendingCreateX_   = canvasPos.x;
                pendingCreateY_   = canvasPos.y;
                createSearch_[0] = '\0';
                ed::Suspend();
                ImGui::OpenPopup("##create_node_popup");
                ed::Resume();
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
            if (ed::AcceptDeletedItem()) {
                const std::uintptr_t raw = nid.Get();
                if (raw & (std::uintptr_t{1} << 62)) {
                    const std::uint32_t cidRaw = static_cast<std::uint32_t>(raw & 0xFFFFFFFFu);
                    model.deleteComment(cidRaw);
                    placedComments_.erase(cidRaw);
                    commentOffX_.erase(cidRaw);
                    commentOffY_.erase(cidRaw);
                } else {
                    model.deleteNode(static_cast<NodeId>(raw));
                }
            }
        }
    }
    ed::EndDelete();

    {
        ed::NodeId ctxNode = 0; ed::PinId ctxPin = 0; ed::LinkId ctxLink = 0;
        ed::Suspend();
        if (ed::ShowNodeContextMenu(&ctxNode)) {
            ctxMenuNode_ = static_cast<unsigned long long>(ctxNode.Get());
            ImGui::OpenPopup("##node_ctx");
        } else if (ed::ShowPinContextMenu(&ctxPin)) {
            ctxMenuPin_ = static_cast<unsigned long long>(ctxPin.Get());
            ImGui::OpenPopup("##pin_ctx");
        } else if (ed::ShowLinkContextMenu(&ctxLink)) {
            // Capture the link's target {toNode,toPort} NOW (stable) rather than
            // re-indexing connections() when the user clicks Delete (the vector
            // may shift before then).
            const std::size_t idx = static_cast<std::size_t>(ctxLink.Get()) - 1;
            const auto& cs = model.graph().connections();
            if (idx < cs.size()) {
                ctxMenuLinkToNode_ = cs[idx].toNode;
                ctxMenuLinkToPort_ = cs[idx].toPort;
            } else {
                ctxMenuLinkToNode_ = 0;
                ctxMenuLinkToPort_.clear();
            }
            ImGui::OpenPopup("##link_ctx");
        } else if (ed::ShowBackgroundContextMenu()) {
            const ImVec2 cp = ed::ScreenToCanvas(ImGui::GetMousePos());
            ctxMenuBgX_ = cp.x; ctxMenuBgY_ = cp.y;
            createSearch_[0] = '\0';
            ImGui::OpenPopup("##bg_ctx");
        }
        ed::Resume();
    }

    // M59: create + context popups. Suspend/Resume + BeginPopup MUST run INSIDE
    // ed::Begin/End (Suspend requires an active canvas draw list); ed::End() below
    // then flushes the suspended popups. Rendering them after ed::End() trips
    // "Suspend was called outside of Begin/End".
    ed::Suspend();
    if (ImGui::BeginPopup("##create_node_popup")) {
        ImGui::TextDisabled("Create node");
        NodeId pn; std::string pp;
        const PortDesc* sp = resolvePin(model, static_cast<std::uintptr_t>(pendingCreatePin_), pn, pp);
        if (sp) {
            const auto options = model.compatibleCreations(sp->type, sp->dir);
            std::vector<std::string> allowedNames;
            allowedNames.reserve(options.size());
            for (const auto& o : options) allowedNames.push_back(o.typeName);
            const std::string pick = drawCreateList(model, createSearch_, sizeof(createSearch_), &allowedNames);
            if (!pick.empty()) {
                std::string targetPort;
                for (const auto& o : options) if (o.typeName == pick) { targetPort = o.targetPort; break; }
                const NodeId nn = model.addNode(pick, pendingCreateX_, pendingCreateY_);
                if (sp->dir == PortDir::Out) model.connect(pn, pp, nn, targetPort);
                else                         model.connect(nn, targetPort, pn, pp);
                createSearch_[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
        } else {
            // Source pin vanished (e.g. undo deleted its node while the popup was
            // open) — don't leave a blank popup; close it.
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // M59: right-click context menus (background / node / pin / link).
    if (ImGui::BeginPopup("##bg_ctx")) {
        ImGui::TextDisabled("Add node");
        const std::string pick = drawCreateList(model, createSearch_, sizeof(createSearch_), nullptr);
        if (!pick.empty()) {
            model.addNode(pick, ctxMenuBgX_, ctxMenuBgY_);
            createSearch_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Add Comment"))
            model.addComment(ctxMenuBgX_, ctxMenuBgY_, 240.0f, 160.0f, "Comment");
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##node_ctx")) {
        const std::uintptr_t raw = static_cast<std::uintptr_t>(ctxMenuNode_);
        if (raw & (std::uintptr_t{1} << 62)) {   // comment id namespace
            if (ImGui::MenuItem("Delete Comment")) {
                const std::uint32_t cid = static_cast<std::uint32_t>(raw & 0xFFFFFFFFu);
                model.deleteComment(cid);
                placedComments_.erase(cid); commentOffX_.erase(cid); commentOffY_.erase(cid);
            }
        } else {
            const NodeId nid = static_cast<NodeId>(raw);
            if (ImGui::MenuItem("Delete")) model.deleteNode(nid);
            if (ImGui::MenuItem("Duplicate")) {
                if (const Node* src = model.graph().node(nid)) {
                    const NodeId dup = model.addNode(src->typeName,
                                                     src->editorX + 30.0f, src->editorY + 30.0f);
                    for (const auto& kv : src->literals) model.setLiteral(dup, kv.first, kv.second);
                }
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##pin_ctx")) {
        NodeId pn; std::string pp;
        const PortDesc* sp = resolvePin(model, static_cast<std::uintptr_t>(ctxMenuPin_), pn, pp);
        if (sp && ImGui::MenuItem("Break links")) {
            if (sp->dir == PortDir::In) model.disconnect(pn, pp);
            else                        model.disconnectOutgoing(pn, pp);
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##link_ctx")) {
        if (!ctxMenuLinkToPort_.empty() && ImGui::MenuItem("Delete link"))
            model.disconnect(ctxMenuLinkToNode_, ctxMenuLinkToPort_);
        ImGui::EndPopup();
    }
    ed::Resume();

    ed::End();
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
    return action;
}

}  // namespace iron
