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
    if (g_failures == 0) std::printf("All world tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
