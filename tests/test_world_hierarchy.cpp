#include "world/World.h"
#include "world/Transform.h"
#include "world/Parent.h"
#include "world/WorldHierarchy.h"
#include "test_framework.h"

#include <cmath>
#include <cstdint>
#include <unordered_map>

static void test_world_matrix_composes_through_parent() {
    iron::World w;
    iron::EntityId p = w.create();
    iron::Transform pt; pt.position = {5, 0, 0};
    w.add<iron::Transform>(p, pt);

    iron::EntityId c = w.create();
    iron::Transform ct; ct.position = {2, 0, 0};
    w.add<iron::Transform>(c, ct);
    w.add<iron::Parent>(c, iron::Parent{p});

    const iron::Mat4 m = iron::worldMatrix(w, c);
    CHECK(std::fabs(m.at(0, 3) - 7.0f) < 1e-5f);   // 5 + 2
}

static void test_world_matrix_root_is_local() {
    iron::World w;
    iron::EntityId e = w.create();
    iron::Transform t; t.position = {3, 4, 0};
    w.add<iron::Transform>(e, t);
    const iron::Mat4 m = iron::worldMatrix(w, e);
    CHECK(std::fabs(m.at(0, 3) - 3.0f) < 1e-5f);
    CHECK(std::fabs(m.at(1, 3) - 4.0f) < 1e-5f);
}

static void test_world_matrix_memoized_matches_plain() {
    iron::World w;
    iron::EntityId p = w.create();
    iron::Transform pt; pt.position = {5, 0, 0};
    w.add<iron::Transform>(p, pt);

    iron::EntityId c = w.create();
    iron::Transform ct; ct.position = {2, 0, 0};
    w.add<iron::Transform>(c, ct);
    w.add<iron::Parent>(c, iron::Parent{p});

    std::unordered_map<std::uint32_t, iron::Mat4> memo;
    const iron::Mat4 m1 = iron::worldMatrix(w, c, memo);
    CHECK(std::fabs(m1.at(0, 3) - 7.0f) < 1e-5f);
    CHECK(memo.size() == 2);   // parent cached too

    // Second call hits the memo and returns the same result.
    const iron::Mat4 m2 = iron::worldMatrix(w, c, memo);
    CHECK(std::fabs(m2.at(0, 3) - 7.0f) < 1e-5f);
    CHECK(memo.size() == 2);
}

int main() {
    test_world_matrix_composes_through_parent();
    test_world_matrix_root_is_local();
    test_world_matrix_memoized_matches_plain();
    return iron_test_result();
}
