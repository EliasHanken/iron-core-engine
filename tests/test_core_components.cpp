#include "scene/RegisterCoreComponents.h"
#include "reflection/RegisterCoreTypes.h"
#include "reflection/Reflection.h"
#include "world/ComponentRegistry.h"
#include "world/CollisionShape.h"
#include "audio/AudioEmitter.h"
#include "render/ReflectionProbe.h"
#include "gameplay/LogicGraphComponent.h"
#include "gameplay/SpawnPoint.h"
#include "test_framework.h"

using namespace iron;

int main() {
    Reflection r;
    registerCollisionShape(r);
    registerAudioEmitter(r);
    registerReflectionProbe(r);
    registerLogicGraphComponent(r);
    registerHealth(r);
    registerSpawnPoint(r);   // M71

    ComponentRegistry reg;
    registerCoreComponents(reg, r);

    CHECK(reg.byName("CollisionShape")     != nullptr);
    CHECK(reg.byName("AudioEmitter")       != nullptr);
    CHECK(reg.byName("ReflectionProbeDef") != nullptr);
    CHECK(reg.byName("LogicGraphComponent")!= nullptr);
    CHECK(reg.byName("Health")             != nullptr);            // M68
    CHECK(reg.byName("SpawnPoint")         != nullptr);            // M71
    CHECK(reg.byTypeId(componentTypeId<CollisionShape>()) != nullptr);
    CHECK(reg.order().size() == 6u);                               // M71: +SpawnPoint

    // M71: SpawnPoint has group (string) + enabled (bool).
    CHECK(reg.byName("SpawnPoint")->fields.size() == 2u);

    // LogicGraphComponent has its one reflected string field.
    CHECK(reg.byName("LogicGraphComponent")->fields.size() == 1u);

    // M68: Health has current + max, neither hidden nor readOnly.
    CHECK(reg.byName("Health")->fields.size() == 2u);
    CHECK(!reg.byName("Health")->fields[0].meta.readOnly);
    CHECK(!reg.byName("Health")->fields[1].meta.readOnly);

    return iron_test_result();
}
