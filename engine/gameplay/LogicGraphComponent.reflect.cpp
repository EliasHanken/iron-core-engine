#include "gameplay/LogicGraphComponent.h"
#include "reflection/Reflection.h"

namespace iron {

void registerLogicGraphComponent(Reflection& r) {
    r.registerType<LogicGraphComponent>("LogicGraphComponent")
        .field("graph", &LogicGraphComponent::graph);
}

}  // namespace iron
