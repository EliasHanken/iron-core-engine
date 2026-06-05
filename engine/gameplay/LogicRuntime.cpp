#include "gameplay/LogicRuntime.h"

#include "gameplay/GameContext.h"
#include "gameplay/LogicGraph.h"
#include "nodes/GraphEvaluator.h"
#include "nodes/NodeContext.h"
#include "world/World.h"

#include <utility>

namespace iron {

void tickLogicGraphs(World& world, const NodeRegistry& registry,
                     float time, float deltaTime) {
    world.view<LogicGraph>().forEach([&](EntityId e, LogicGraph& lg) {
        GameContext gc{&world, e, time, deltaTime};
        RunContext ctx;
        ctx.vars = std::move(lg.vars);     // restore persistent state
        ctx.domainContext = &gc;
        run(lg.graph, registry, ctx);
        lg.vars = std::move(ctx.vars);     // keep mutated state for next tick
    });
}

}  // namespace iron
