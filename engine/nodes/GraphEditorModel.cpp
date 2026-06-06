#include "nodes/GraphEditorModel.h"

#include "nodes/GraphEvaluator.h"
#include "nodes/NodeGraphIO.h"
#include "nodes/NodeRegistry.h"

#include <algorithm>   // std::remove_if, std::max
#include <cmath>       // std::fabs
#include <utility>     // std::move

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

bool GraphEditorModel::connect(NodeId fromNode, std::string fromPort,
                               NodeId toNode, std::string toPort) {
    const PortDesc* from = portOf(fromNode, fromPort);
    const PortDesc* to   = portOf(toNode, toPort);
    if (!from || !to) return false;
    if (!portsCompatible(*from, *to)) return false;

    if (from->type == PortType::Exec) {
        graph_.removeOutgoing(fromNode, fromPort);        // exec out: one target
    } else {
        graph_.disconnect(toNode, toPort);                // data in: one source
    }
    graph_.connect(fromNode, std::move(fromPort), toNode, std::move(toPort));
    dirty_ = true;
    return true;
}

std::vector<NodeCreation> GraphEditorModel::compatibleCreations(PortType srcType,
                                                                PortDir srcDir) const {
    std::vector<NodeCreation> out;
    if (!registry_) return out;
    const PortDesc src{"", srcType, srcDir};
    for (const NodeTypeDesc* t : registry_->all()) {
        for (const PortDesc& p : t->ports) {
            const bool ok = (srcDir == PortDir::Out) ? portsCompatible(src, p)
                                                     : portsCompatible(p, src);
            if (ok) { out.push_back({t->typeName, p.name}); break; }  // first match per type
        }
    }
    return out;
}

void GraphEditorModel::disconnect(NodeId toNode, std::string toPort) {
    graph_.disconnect(toNode, toPort);
    dirty_ = true;
}

void GraphEditorModel::disconnectOutgoing(NodeId fromNode, std::string fromPort) {
    graph_.removeOutgoing(fromNode, fromPort);
    dirty_ = true;
}

void GraphEditorModel::setLiteral(NodeId id, std::string port, NodeValue value) {
    graph_.setLiteral(id, std::move(port), std::move(value));
    dirty_ = true;
}

void GraphEditorModel::setNodePosition(NodeId id, float x, float y) {
    if (Node* n = graph_.node(id)) {
        n->editorX = x;
        n->editorY = y;
        dirty_ = true;
    }
}

std::uint32_t GraphEditorModel::addComment(float x, float y, float w, float h, std::string title) {
    Comment c;
    c.id = nextCommentId_++;
    c.x = x; c.y = y; c.w = w; c.h = h;
    c.title = std::move(title);
    comments_.push_back(std::move(c));
    dirty_ = true;
    return comments_.back().id;
}

void GraphEditorModel::deleteComment(std::uint32_t id) {
    const auto it = std::remove_if(comments_.begin(), comments_.end(),
                                   [id](const Comment& c) { return c.id == id; });
    if (it != comments_.end()) { comments_.erase(it, comments_.end()); dirty_ = true; }
}

void GraphEditorModel::setCommentRect(std::uint32_t id, float x, float y, float w, float h) {
    constexpr float kTol = 0.5f;   // half a pixel; avoids per-frame dirtying from
                                   // sub-pixel float drift in the panel's size math
    for (Comment& c : comments_) {
        if (c.id != id) continue;
        if (std::fabs(c.x - x) > kTol || std::fabs(c.y - y) > kTol ||
            std::fabs(c.w - w) > kTol || std::fabs(c.h - h) > kTol) {
            c.x = x; c.y = y; c.w = w; c.h = h;
            dirty_ = true;
        }
        return;
    }
}

void GraphEditorModel::setCommentTitle(std::uint32_t id, std::string title) {
    for (Comment& c : comments_) {
        if (c.id != id) continue;
        if (c.title != title) { c.title = std::move(title); dirty_ = true; }
        return;
    }
}

void GraphEditorModel::run() {
    lastRun_ = RunContext{};
    if (registry_) iron::run(graph_, *registry_, lastRun_);
}

nlohmann::json GraphEditorModel::toJson() const {
    nlohmann::json j = iron::toJson(graph_);
    nlohmann::json arr = nlohmann::json::array();
    for (const Comment& c : comments_) {
        arr.push_back({{"id", c.id}, {"x", c.x}, {"y", c.y},
                       {"w", c.w}, {"h", c.h}, {"title", c.title}});
    }
    j["comments"]      = arr;
    j["nextCommentId"] = nextCommentId_;
    return j;
}

bool GraphEditorModel::loadFromJson(const nlohmann::json& j) {
    if (!registry_) return false;
    auto g = iron::fromJson(j, *registry_);
    if (!g) return false;
    graph_ = std::move(*g);
    selected_ = 0;

    comments_.clear();
    nextCommentId_ = 1;
    if (j.contains("comments") && j["comments"].is_array()) {
        for (const auto& jc : j["comments"]) {
            Comment c;
            c.id    = jc.value("id", 0u);
            c.x     = jc.value("x", 0.0f);
            c.y     = jc.value("y", 0.0f);
            c.w     = jc.value("w", 240.0f);
            c.h     = jc.value("h", 160.0f);
            c.title = jc.value("title", std::string("Comment"));
            if (c.id == 0) continue;   // skip malformed
            comments_.push_back(std::move(c));
            nextCommentId_ = std::max(nextCommentId_, c.id + 1);
        }
    }
    nextCommentId_ = std::max(nextCommentId_, j.value("nextCommentId", 1u));

    dirty_ = false;
    return true;
}

}  // namespace iron
