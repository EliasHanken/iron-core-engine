#include "scene/Prefab.h"
#include "scene/PrefabIO.h"
#include "scene/SceneFormat.h"
#include "reflection/Reflection.h"
#include "reflection/RegisterCoreTypes.h"
#include "scene/RegisterCoreComponents.h"
#include "world/CollisionShape.h"
#include "world/ComponentRegistry.h"
#include "test_framework.h"

#include <string>

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
    return r;
}

static void test_roundtrip_hierarchy_with_component() {
    iron::Reflection r = makeReflection();
    iron::ComponentRegistry cr;
    iron::registerCoreComponents(cr, r);

    iron::Prefab p;
    iron::SceneEntity root; root.name = "turret"; root.transform.position = {2, 0, 0};
    iron::SceneEntity barrel; barrel.name = "barrel"; barrel.parentIndex = 0;
    barrel.transform.position = {0, 1, 0};
    iron::CollisionShape cs; cs.body = iron::ColliderBody::Dynamic;
    barrel.components.add<iron::CollisionShape>(cs);
    p.entities = {root, barrel};

    const std::string js = iron::prefabToJsonString(r, cr, p);
    auto loaded = iron::prefabFromJsonString(r, cr, js);
    CHECK(loaded.has_value());
    CHECK(loaded->entities.size() == 2);
    CHECK(loaded->entities[0].name == "turret");
    CHECK(loaded->entities[0].parentIndex == -1);
    CHECK(loaded->entities[1].name == "barrel");
    CHECK(loaded->entities[1].parentIndex == 0);
    CHECK_NEAR(loaded->entities[1].transform.position.y, 1.0f);
    const iron::CollisionShape* lc = loaded->entities[1].components.get<iron::CollisionShape>();
    CHECK(lc != nullptr);
    CHECK(lc && lc->body == iron::ColliderBody::Dynamic);
}

static void test_load_sanitizes_bad_parent() {
    iron::Reflection r = makeReflection();
    iron::ComponentRegistry cr;
    iron::registerCoreComponents(cr, r);
    const char* bad =
        R"({"version":1,"entities":[{"name":"a","parent":5},{"name":"b","parent":7}]})";
    auto loaded = iron::prefabFromJsonString(r, cr, bad);
    CHECK(loaded.has_value());
    CHECK(loaded->entities.size() == 2);
    CHECK(loaded->entities[0].parentIndex == -1);   // root forced to -1
    CHECK(loaded->entities[1].parentIndex == -1);   // out-of-range sanitized

    // Self-parent (pi == i) is cleared.
    const char* selfParent =
        R"({"version":1,"entities":[{"name":"a"},{"name":"b","parent":1}]})";
    auto sp = iron::prefabFromJsonString(r, cr, selfParent);
    CHECK(sp.has_value());
    CHECK(sp->entities[1].parentIndex == -1);   // self-ref cleared

    // Mutual cycle (1 -> 2 -> 1) is broken (at least one link reset to root).
    const char* cycle =
        R"({"version":1,"entities":[{"name":"a"},{"name":"b","parent":2},{"name":"c","parent":1}]})";
    auto cy = iron::prefabFromJsonString(r, cr, cycle);
    CHECK(cy.has_value());
    CHECK(cy->entities[1].parentIndex == -1 || cy->entities[2].parentIndex == -1);
}

static void test_empty_or_malformed_is_nullopt() {
    iron::Reflection r = makeReflection();
    iron::ComponentRegistry cr;
    iron::registerCoreComponents(cr, r);
    CHECK(!iron::prefabFromJsonString(r, cr, "not json").has_value());
    CHECK(!iron::prefabFromJsonString(r, cr, R"({"version":1,"entities":[]})").has_value());
    CHECK(!iron::prefabFromJsonString(r, cr, R"({"version":1})").has_value());
}

int main() {
    test_roundtrip_hierarchy_with_component();
    test_load_sanitizes_bad_parent();
    test_empty_or_malformed_is_nullopt();
    return iron_test_result();
}
