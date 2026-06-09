#include "scene/RegisterCoreComponents.h"

#include "audio/AudioEmitter.h"
#include "gameplay/Health.h"
#include "gameplay/LogicGraphComponent.h"
#include "render/ReflectionProbe.h"
#include "world/CollisionShape.h"
#include "world/ComponentRegistry.h"

namespace iron {

void registerCoreComponents(ComponentRegistry& cr, const Reflection& r) {
    cr.registerComponent<CollisionShape>("CollisionShape", r);
    cr.registerComponent<AudioEmitter>("AudioEmitter", r);
    cr.registerComponent<ReflectionProbeDef>("ReflectionProbeDef", r);
    cr.registerComponent<LogicGraphComponent>("LogicGraphComponent", r);
    cr.registerComponent<Health>("Health", r);   // M68 demo gameplay component
}

}  // namespace iron
