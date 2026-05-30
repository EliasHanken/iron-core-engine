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

int main() {
    test_entityid_default_is_invalid();
    test_entityid_with_generation_is_valid();
    test_component_type_ids_are_distinct();
    if (g_failures == 0) std::printf("All world tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
