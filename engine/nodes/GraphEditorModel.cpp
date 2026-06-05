#include "nodes/GraphEditorModel.h"

#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraphIO.h"
#include "nodes/NodeRegistry.h"

namespace iron {

GraphEditorModel::GraphEditorModel(const NodeRegistry* registry)
    : registry_(registry) {}

const PortDesc* GraphEditorModel::portOf(NodeId node, std::string_view port) const {
    const Node* n = graph_.node(node);
    if (!n || !registry_) return nullptr;
    const NodeTypeDesc* t = registry_->find(n->typeName);
    if (!t) return nullptr;
    for (const PortDesc& p : t->ports)
        if (p.name == port) return &p;
    return nullptr;
}

NodeId GraphEditorModel::addNode(std::string typeName, float x, float y) {
    const NodeId id = graph_.addNode(std::move(typeName));
    if (Node* n = graph_.node(id)) { n->editorX = x; n->editorY = y; }
    dirty_ = true;
    return id;
}

void GraphEditorModel::deleteNode(NodeId id) {
    graph_.removeNode(id);
    if (selected_ == id) selected_ = 0;
    dirty_ = true;
}

namespace {
bool dataCompatible(PortType a, PortType b) {
    if (a == b) return true;
    const bool aNum = (a == PortType::Int || a == PortType::Float);
    const bool bNum = (b == PortType::Int || b == PortType::Float);
    return aNum && bNum;
}
}  // namespace

bool GraphEditorModel::connect(NodeId fromNode, std::string fromPort,
                               NodeId toNode, std::string toPort) {
    const PortDesc* from = portOf(fromNode, fromPort);
    const PortDesc* to   = portOf(toNode, toPort);
    if (!from || !to) return false;
    if (from->dir != PortDir::Out || to->dir != PortDir::In) return false;

    const bool fromExec = from->type == PortType::Exec;
    const bool toExec   = to->type == PortType::Exec;
    if (fromExec != toExec) return false;                 // exec<->data mismatch
    if (!fromExec && !dataCompatible(from->type, to->type)) return false;

    if (fromExec) {
        graph_.removeOutgoing(fromNode, fromPort);        // exec out: one target
    } else {
        graph_.disconnect(toNode, toPort);                // data in: one source
    }
    graph_.connect(fromNode, std::move(fromPort), toNode, std::move(toPort));
    dirty_ = true;
    return true;
}

void GraphEditorModel::disconnect(NodeId toNode, std::string toPort) {
    graph_.disconnect(toNode, toPort);
    dirty_ = true;
}

void GraphEditorModel::setLiteral(NodeId id, std::string port, NodeValue value) {
    graph_.setLiteral(id, std::move(port), std::move(value));
    dirty_ = true;
}

void GraphEditorModel::run() {
    lastRun_ = RunContext{};
    if (registry_) iron::run(graph_, *registry_, lastRun_);
}

nlohmann::json GraphEditorModel::toJson() const { return iron::toJson(graph_); }

bool GraphEditorModel::loadFromJson(const nlohmann::json& j) {
    if (!registry_) return false;
    auto g = iron::fromJson(j, *registry_);
    if (!g) return false;
    graph_ = std::move(*g);
    selected_ = 0;
    dirty_ = false;
    return true;
}

}  // namespace iron
