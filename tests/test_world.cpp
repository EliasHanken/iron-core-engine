#include "world/Entity.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void test_entityid_default_is_invalid() {
    iron::EntityId e;
    CHECK(!e.valid());
    CHECK(e == iron::kEntityNone);
}

static void test_entityid_with_generation_is_valid() {
    iron::EntityId e{0, 1};
    CHECK(e.valid());
    CHECK(!(e == iron::kEntityNone));
}

static void test_component_type_ids_are_distinct() {
    const auto a = iron::componentTypeId<int>();
    const auto b = iron::componentTypeId<float>();
    const auto a2 = iron::componentTypeId<int>();
    CHECK(a != b);
    CHECK(a == a2);
}

#include "world/ComponentArray.h"

static void test_component_array_add_and_get() {
    iron::ComponentArray<int> arr;
    iron::EntityId e{0, 1};
    int* p = arr.add(e, 42);
    CHECK(p != nullptr);
    CHECK(*p == 42);
    CHECK(arr.get(e) != nullptr);
    CHECK(*arr.get(e) == 42);
    CHECK(arr.size() == 1);
}

static void test_component_array_get_missing_returns_null() {
    iron::ComponentArray<int> arr;
    iron::EntityId e{0, 1};
    CHECK(arr.get(e) == nullptr);
}

static void test_component_array_add_two_entities() {
    iron::ComponentArray<int> arr;
    iron::EntityId a{0, 1};
    iron::EntityId b{1, 1};
    arr.add(a, 10);
    arr.add(b, 20);
    CHECK(arr.size() == 2);
    CHECK(*arr.get(a) == 10);
    CHECK(*arr.get(b) == 20);
}

static void test_component_array_remove_invalidates_get() {
    iron::ComponentArray<int> arr;
    iron::EntityId e{0, 1};
    arr.add(e, 7);
    arr.remove(e);
    CHECK(arr.get(e) == nullptr);
    CHECK(arr.size() == 0);
}

static void test_component_array_remove_swaps_correctly() {
    // Insert A,B,C; remove B; verify C's sparse index points at row 1 (B's old row).
    iron::ComponentArray<int> arr;
    iron::EntityId a{0, 1};
    iron::EntityId b{1, 1};
    iron::EntityId c{2, 1};
    arr.add(a, 10);
    arr.add(b, 20);
    arr.add(c, 30);
    arr.remove(b);
    CHECK(arr.size() == 2);
    CHECK(arr.get(b) == nullptr);
    CHECK(*arr.get(a) == 10);
    CHECK(*arr.get(c) == 30);  // sparse for c must be updated after the swap
}

static void test_component_array_remove_then_readd() {
    iron::ComponentArray<int> arr;
    iron::EntityId e{0, 1};
    arr.add(e, 7);
    arr.remove(e);
    arr.add(e, 99);
    CHECK(arr.size() == 1);
    CHECK(*arr.get(e) == 99);
}

static void test_component_array_operator_index() {
    iron::ComponentArray<int> arr;
    iron::EntityId a{0, 1};
    iron::EntityId b{1, 1};
    arr.add(a, 10);
    arr.add(b, 20);
    CHECK(arr[0] == 10);
    CHECK(arr[1] == 20);
}

static void test_component_array_entity_at() {
    iron::ComponentArray<int> arr;
    iron::EntityId a{0, 1};
    iron::EntityId b{1, 1};
    arr.add(a, 10);
    arr.add(b, 20);
    CHECK(arr.entityAt(0) == a);
    CHECK(arr.entityAt(1) == b);
}

static void test_component_array_iteration_after_swap() {
    // Insert A,B,C; remove B; verify rows 0,1 are A=10, C=30.
    iron::ComponentArray<int> arr;
    iron::EntityId a{0, 1};
    iron::EntityId b{1, 1};
    iron::EntityId c{2, 1};
    arr.add(a, 10);
    arr.add(b, 20);
    arr.add(c, 30);
    arr.remove(b);
    CHECK(arr.size() == 2);
    CHECK(arr[0] == 10);
    CHECK(arr[1] == 30);
    CHECK(arr.entityAt(0) == a);
    CHECK(arr.entityAt(1) == c);
}

#include "world/World.h"

static void test_world_create_returns_valid_entity() {
    iron::World w;
    iron::EntityId e = w.create();
    CHECK(e.valid());
    CHECK(w.alive(e));
}

static void test_world_destroy_kills_entity() {
    iron::World w;
    iron::EntityId e = w.create();
    w.destroy(e);
    CHECK(!w.alive(e));
}

static void test_world_recycle_bumps_generation() {
    iron::World w;
    iron::EntityId a = w.create();
    w.destroy(a);
    iron::EntityId b = w.create();
    CHECK(b.valid());
    CHECK(b.index == a.index);   // slot reused
    CHECK(b.generation != a.generation);
    CHECK(!w.alive(a));          // stale handle stays dead
    CHECK(w.alive(b));
}

static void test_world_typed_add_and_get() {
    iron::World w;
    iron::EntityId e = w.create();
    int* p = w.add<int>(e, 42);
    CHECK(p != nullptr);
    CHECK(*w.get<int>(e) == 42);
}

static void test_world_typed_remove() {
    iron::World w;
    iron::EntityId e = w.create();
    w.add<int>(e, 42);
    w.remove<int>(e);
    CHECK(w.get<int>(e) == nullptr);
}

static void test_world_multi_type_on_same_entity() {
    iron::World w;
    iron::EntityId e = w.create();
    w.add<int>(e, 7);
    w.add<float>(e, 3.5f);
    CHECK(*w.get<int>(e) == 7);
    CHECK(*w.get<float>(e) == 3.5f);
}

static void test_world_destroy_tears_off_all_components() {
    iron::World w;
    iron::EntityId e = w.create();
    w.add<int>(e, 7);
    w.add<float>(e, 3.5f);
    w.destroy(e);
    CHECK(w.get<int>(e) == nullptr);
    CHECK(w.get<float>(e) == nullptr);
}

static void test_world_view_size_matches_components_present() {
    iron::World w;
    iron::EntityId a = w.create();
    iron::EntityId b = w.create();
    w.add<int>(a, 10);
    w.add<int>(b, 20);
    auto& v = w.view<int>();
    CHECK(v.size() == 2);
}

static void test_world_view_iteration_and_entity_at() {
    iron::World w;
    iron::EntityId a = w.create();
    iron::EntityId b = w.create();
    w.add<int>(a, 10);
    w.add<int>(b, 20);
    auto& v = w.view<int>();
    CHECK(v[0] == 10 || v[0] == 20);   // order is insertion-modulo-swaps
    CHECK(v.entityAt(0) == a || v.entityAt(0) == b);
}

static void test_world_view_empty_when_no_component_of_type() {
    iron::World w;
    auto& v = w.view<int>();
    CHECK(v.size() == 0);
}

#include "world/Transform.h"

static void test_transform_component_roundtrip() {
    iron::World w;
    iron::EntityId e = w.create();
    iron::Transform t{};
    t.position = iron::Vec3{1, 2, 3};
    t.scale    = iron::Vec3{2, 2, 2};
    w.add<iron::Transform>(e, t);
    auto* got = w.get<iron::Transform>(e);
    CHECK(got != nullptr);
    CHECK(got->position.x == 1.0f);
    CHECK(got->position.y == 2.0f);
    CHECK(got->position.z == 3.0f);
    CHECK(got->scale.x    == 2.0f);
}

#include "render/RenderHandles.h"
#include "scene/SceneFormat.h"   // MeshRef, MaterialDef

static void test_render_handles_component_roundtrip() {
    iron::World w;
    iron::EntityId e = w.create();
    iron::RenderHandles rh{};
    rh.mesh     = 7;
    rh.albedo   = 11;
    rh.normal   = 13;
    rh.specular = 17;
    w.add<iron::RenderHandles>(e, rh);
    auto* got = w.get<iron::RenderHandles>(e);
    CHECK(got != nullptr);
    CHECK(got->mesh   == 7u);
    CHECK(got->albedo == 11u);
    CHECK(got->normal == 13u);
    CHECK(got->specular == 17u);
}

static void test_world_render_submit_pseudocode() {
    iron::World w;
    struct SubmitEntry {
        iron::Vec3       pos;
        iron::MeshHandle mesh;
    };
    std::vector<SubmitEntry> submitted;

    // Build three renderable entities.
    auto makeEntity = [&](iron::Vec3 pos, iron::MeshHandle m,
                          iron::TextureHandle a, float emissiveR) {
        iron::EntityId e = w.create();
        iron::Transform t{};      t.position = pos;
        w.add<iron::Transform>(e, t);
        iron::MeshRef ref{};      // empty primitive/path — fine for the test
        w.add<iron::MeshRef>(e, ref);
        iron::MaterialDef mat{};  mat.emissive = iron::Vec3{emissiveR, 0, 0};
        w.add<iron::MaterialDef>(e, mat);
        iron::RenderHandles rh{}; rh.mesh = m; rh.albedo = a;
        w.add<iron::RenderHandles>(e, rh);
    };
    makeEntity(iron::Vec3{1, 0, 0}, 100, 200, 0.1f);
    makeEntity(iron::Vec3{2, 0, 0}, 101, 201, 0.2f);
    makeEntity(iron::Vec3{3, 0, 0}, 102, 202, 0.3f);

    // Walk Section 3 submit pseudocode: iterate Transform view, look up siblings.
    auto& transforms = w.view<iron::Transform>();
    for (size_t row = 0; row < transforms.size(); ++row) {
        iron::EntityId e = transforms.entityAt(row);
        const iron::Transform&     t   = transforms[row];
        const iron::MeshRef*       mr  = w.get<iron::MeshRef>(e);
        const iron::MaterialDef*   mat = w.get<iron::MaterialDef>(e);
        const iron::RenderHandles* rh  = w.get<iron::RenderHandles>(e);
        if (!mr || !mat || !rh) continue;
        submitted.push_back({t.position, rh->mesh});
    }

    CHECK(submitted.size() == 3);
    CHECK(submitted[0].pos.x  == 1.0f);
    CHECK(submitted[0].mesh   == 100u);
    CHECK(submitted[2].mesh   == 102u);
}

int main() {
    test_entityid_default_is_invalid();
    test_entityid_with_generation_is_valid();
    test_component_type_ids_are_distinct();
    test_component_array_add_and_get();
    test_component_array_get_missing_returns_null();
    test_component_array_add_two_entities();
    test_component_array_remove_invalidates_get();
    test_component_array_remove_swaps_correctly();
    test_component_array_remove_then_readd();
    test_component_array_operator_index();
    test_component_array_entity_at();
    test_component_array_iteration_after_swap();
    test_world_create_returns_valid_entity();
    test_world_destroy_kills_entity();
    test_world_recycle_bumps_generation();
    test_world_typed_add_and_get();
    test_world_typed_remove();
    test_world_multi_type_on_same_entity();
    test_world_destroy_tears_off_all_components();
    test_world_view_size_matches_components_present();
    test_world_view_iteration_and_entity_at();
    test_world_view_empty_when_no_component_of_type();
    test_transform_component_roundtrip();
    test_render_handles_component_roundtrip();
    test_world_render_submit_pseudocode();
    if (g_failures == 0) std::printf("All world tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
