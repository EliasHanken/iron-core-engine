#include "nodes/GraphEvaluator.h"

#include "nodes/NodeGraph.h"
#include "nodes/NodeRegistry.h"
#include "core/Log.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace iron {

namespace {

std::string key(NodeId id, std::string_view port) {
    return std::to_string(id) + "/" + std::string(port);
}

class Evaluator;

class Ctx final : public NodeContext {
public:
    Ctx(Evaluator& ev, NodeId node) : ev_(ev), node_(node) {}
    const NodeValue& in(std::string_view port) override;
    void out(std::string_view port, NodeValue value) override;
    void fire(std::string_view execPort) override { fired.emplace_back(execPort); }
    RunContext& run() override;

    std::vector<std::string> fired;       // exec outs fired this evaluate
private:
    Evaluator& ev_;
    NodeId node_;
    std::unordered_map<std::string, NodeValue> inCache_;  // stable refs for in()
};

class Evaluator {
public:
    Evaluator(const Graph& g, const NodeRegistry& r, RunContext& ctx)
        : g_(g), r_(r), ctx_(ctx) {}

    void runExec(NodeId nodeId) {
        if (++steps_ > ctx_.maxSteps) {
            if (!warnedSteps_) {
                Log::warn("GraphEvaluator: exceeded maxSteps (%d) - possible "
                          "exec cycle; halting", ctx_.maxSteps);
                warnedSteps_ = true;
            }
            return;
        }
        const Node* n = g_.node(nodeId);
        if (!n) return;
        const NodeTypeDesc* t = r_.find(n->typeName);
        if (!t || !t->evaluate) return;

        Ctx c(*this, nodeId);
        t->evaluate(c);
        for (const std::string& execOut : c.fired) {
            if (auto conn = g_.outgoing(nodeId, execOut)) runExec(conn->toNode);
        }
    }

    NodeValue pullValue(NodeId nodeId, std::string_view port) {
        const std::string k = key(nodeId, port);
        if (auto it = memo_.find(k); it != memo_.end()) return it->second;
        const Node* n = g_.node(nodeId);
        if (!n) return NodeValue{};
        const NodeTypeDesc* t = r_.find(n->typeName);
        if (!t || !t->evaluate) return NodeValue{};
        Ctx c(*this, nodeId);
        t->evaluate(c);                 // out() writes into memo_
        if (auto it = memo_.find(k); it != memo_.end()) return it->second;
        for (const PortDesc& p : t->ports)
            if (p.name == port) return zeroValue(p.type);
        return NodeValue{};
    }

    NodeValue resolveInput(NodeId nodeId, std::string_view port) {
        if (auto conn = g_.incoming(nodeId, port))
            return pullValue(conn->fromNode, conn->fromPort);
        const Node* n = g_.node(nodeId);
        if (n) {
            auto it = n->literals.find(std::string(port));
            if (it != n->literals.end()) return it->second;
            const NodeTypeDesc* t = r_.find(n->typeName);
            if (t) for (const PortDesc& p : t->ports)
                if (p.name == port) return zeroValue(p.type);
        }
        return NodeValue{};
    }

    void setOutput(NodeId nodeId, std::string_view port, NodeValue v) {
        memo_[key(nodeId, port)] = std::move(v);
    }

    RunContext& ctx() { return ctx_; }

private:
    const Graph& g_;
    const NodeRegistry& r_;
    RunContext& ctx_;
    std::unordered_map<std::string, NodeValue> memo_;  // "id/port" -> output value
    int steps_ = 0;
    bool warnedSteps_ = false;
};

const NodeValue& Ctx::in(std::string_view port) {
    const std::string ks(port);
    inCache_[ks] = ev_.resolveInput(node_, port);
    return inCache_[ks];
}
void Ctx::out(std::string_view port, NodeValue value) {
    ev_.setOutput(node_, port, std::move(value));
}
RunContext& Ctx::run() { return ev_.ctx(); }

}  // namespace

void run(const Graph& graph, const NodeRegistry& registry, RunContext& ctx) {
    NodeId entry = 0;
    for (const Node& n : graph.nodes()) {
        if (n.typeName == "Entry") { entry = n.id; break; }
    }
    if (entry == 0) {
        Log::warn("GraphEvaluator: no Entry node; nothing to run");
        return;
    }
    Evaluator ev(graph, registry, ctx);
    ev.runExec(entry);
}

}  // namespace iron
