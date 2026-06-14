#include "scene/SceneFormat.h"
#include "scene/SceneIO.h"
#include "gameplay/SpawnPoint.h"
#include "reflection/Reflection.h"
#include "reflection/RegisterCoreTypes.h"
#include "scene/RegisterCoreComponents.h"
#include "world/ComponentRegistry.h"
#include "test_framework.h"

static iron::Reflection makeReflection() {
    iron::Reflection r;
    iron::registerTransform(r);
    iron::registerMeshRef(r);
    iron::registerMaterialDef(r);
    iron::registerRenderHandles(r);
    iron::registerCollisionShape(r);
    iron::registerAudioEmitter(r);
    iron::registerReflectionProbe(r);
    iron::registerLogicGraphComponent(r);
    iron::registerHealth(r);
    iron::registerSpawnPoint(r);   // M71
    return r;
}

static void test_spawn_point_roundtrip() {
    iron::Reflection r = makeReflection();
    iron::ComponentRegistry cr;
    iron::registerCoreComponents(cr, r);

    iron::SceneFile s;
    iron::SceneEntity e; e.name = "spawn";
    e.components.add<iron::SpawnPoint>(iron::SpawnPoint{"enemy", false});
    s.entities = {e};

    const std::string js = iron::sceneToJsonString(r, cr, s);
    auto loaded = iron::sceneFromJsonString(r, cr, js);
    CHECK(loaded.has_value());
    CHECK(loaded->entities.size() == 1);
    const iron::SpawnPoint* sp = loaded->entities[0].components.get<iron::SpawnPoint>();
    CHECK(sp != nullptr);
    CHECK(sp && sp->group == "enemy");
    CHECK(sp && sp->enabled == false);
}

int main() {
    test_spawn_point_roundtrip();
    return iron_test_result();
}
