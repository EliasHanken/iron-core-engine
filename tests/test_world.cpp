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

int main() {
    test_entityid_default_is_invalid();
    test_entityid_with_generation_is_valid();
    test_component_type_ids_are_distinct();
    test_component_array_add_and_get();
    test_component_array_get_missing_returns_null();
    test_component_array_add_two_entities();
    if (g_failures == 0) std::printf("All world tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
