#include "world/Transform.h"
#include "math/Transform.h"
#include "scene/SceneHierarchy.h"
#include "test_framework.h"

static void test_transform_matrix_equals_inline_composition() {
    iron::Transform t;
    t.position = {1.0f, 2.0f, 3.0f};
    t.rotation = iron::Quat::fromAxisAngle({0.0f, 1.0f, 0.0f}, 0.7f);
    t.scale    = {2.0f, 0.5f, 1.5f};

    const iron::Mat4 expected = iron::translation(t.position)
                              * t.rotation.toMat4()
                              * iron::scaling(t.scale);
    for (int i = 0; i < 16; ++i)
        CHECK_NEAR(t.matrix().m[i], expected.m[i]);
}

// Build a 3-deep chain root(0) -> mid(1) -> leaf(2), each offset +1 on X.
static iron::SceneFile makeChain() {
    iron::SceneFile s;
    iron::SceneEntity e0; e0.name = "root"; e0.transform.position = {1, 0, 0};
    iron::SceneEntity e1; e1.name = "mid";  e1.transform.position = {1, 0, 0}; e1.parentIndex = 0;
    iron::SceneEntity e2; e2.name = "leaf"; e2.transform.position = {1, 0, 0}; e2.parentIndex = 1;
    s.entities = {e0, e1, e2};
    return s;
}

static void test_world_matrix_three_deep_translation() {
    iron::SceneFile s = makeChain();
    const iron::Mat4 w = iron::worldMatrixOf(s, 2);
    CHECK_NEAR(w.at(0, 3), 3.0f);   // leaf world X = 1+1+1
    CHECK_NEAR(w.at(1, 3), 0.0f);
    CHECK_NEAR(w.at(2, 3), 0.0f);
}

static void test_world_matrix_with_rotation_and_scale() {
    // Parent rotates 90deg about Y and scales x2; child sits at local +X 1.
    iron::SceneFile s;
    iron::SceneEntity p; p.transform.rotation = iron::Quat::fromAxisAngle({0,1,0}, 1.57079633f);
    p.transform.scale = {2,2,2};
    iron::SceneEntity c; c.transform.position = {1,0,0}; c.parentIndex = 0;
    s.entities = {p, c};
    const iron::Mat4 w = iron::worldMatrixOf(s, 1);
    // local +X, scaled x2 -> (2,0,0), rotated +90 about Y -> ~(0,0,-2).
    CHECK_NEAR(w.at(0, 3), 0.0f);
    CHECK_NEAR(w.at(2, 3), -2.0f);
}

static void test_is_descendant() {
    iron::SceneFile s = makeChain();
    CHECK(iron::isDescendant(s, 0, 2));   // leaf under root
    CHECK(iron::isDescendant(s, 1, 2));   // leaf under mid
    CHECK(iron::isDescendant(s, 0, 0));   // self counts
    CHECK(!iron::isDescendant(s, 2, 0));  // root is not under leaf
}

static void test_collect_subtree() {
    iron::SceneFile s = makeChain();
    auto sub = iron::collectSubtree(s, 1);   // mid + leaf
    CHECK(sub.size() == 2u);
    CHECK(sub[0] == 1);                       // root first
    bool hasLeaf = false; for (int i : sub) if (i == 2) hasLeaf = true;
    CHECK(hasLeaf);
}

static void test_cycle_guard_does_not_hang() {
    iron::SceneFile s = makeChain();
    s.entities[0].parentIndex = 2;   // root -> leaf -> mid -> root : cycle
    const iron::Mat4 w = iron::worldMatrixOf(s, 2);   // must return (depth-capped), not hang
    CHECK(w.at(0, 0) == w.at(0, 0));   // finite (not NaN) — NaN != NaN
}

int main() {
    test_transform_matrix_equals_inline_composition();
    test_world_matrix_three_deep_translation();
    test_world_matrix_with_rotation_and_scale();
    test_is_descendant();
    test_collect_subtree();
    test_cycle_guard_does_not_hang();
    return iron_test_result();
}
