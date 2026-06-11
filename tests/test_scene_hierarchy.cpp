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

static void test_reparent_preserves_world_position() {
    iron::SceneFile s;
    iron::SceneEntity p; p.transform.position = {5, 0, 0};
    iron::SceneEntity c; c.transform.position = {7, 0, 0};   // world (7,0,0), root
    s.entities = {p, c};
    const iron::Mat4 before = iron::worldMatrixOf(s, 1);
    CHECK(iron::reparentKeepWorld(s, 1, 0));
    CHECK(s.entities[1].parentIndex == 0);
    const iron::Mat4 after = iron::worldMatrixOf(s, 1);
    CHECK_NEAR(after.at(0,3), before.at(0,3));           // world X unchanged
    CHECK_NEAR(s.entities[1].transform.position.x, 2.0f); // new local = 7 - 5
}

static void test_reparent_to_root_keeps_world() {
    iron::SceneFile s = makeChain();          // root(0)->mid(1)->leaf(2)
    const iron::Mat4 before = iron::worldMatrixOf(s, 2);
    CHECK(iron::reparentKeepWorld(s, 2, -1));  // unparent leaf
    CHECK(s.entities[2].parentIndex == -1);
    const iron::Mat4 after = iron::worldMatrixOf(s, 2);
    CHECK_NEAR(after.at(0,3), before.at(0,3));
    CHECK_NEAR(s.entities[2].transform.position.x, 3.0f);  // was world X=3
}

static void test_reparent_rejections() {
    iron::SceneFile s = makeChain();
    CHECK(!iron::reparentKeepWorld(s, 0, 0));   // self
    CHECK(!iron::reparentKeepWorld(s, 0, 2));   // newParent is descendant of child -> cycle
    CHECK(!iron::reparentKeepWorld(s, 9, 0));   // child out of range
    CHECK(!iron::reparentKeepWorld(s, 1, 9));   // newParent out of range
}

static void test_reparent_onto_current_parent_is_noop() {
    iron::SceneFile s = makeChain();           // root(0)->mid(1)->leaf(2)
    const iron::Transform before = s.entities[2].transform;
    CHECK(!iron::reparentKeepWorld(s, 2, 1));  // leaf already under mid -> rejected
    CHECK(s.entities[2].parentIndex == 1);
    // Bit-identical local transform: no inverse()*decomposeTRS round-trip allowed.
    const iron::Transform& after = s.entities[2].transform;
    CHECK(after.position.x == before.position.x);
    CHECK(after.position.y == before.position.y);
    CHECK(after.position.z == before.position.z);
    CHECK(after.rotation.x == before.rotation.x);
    CHECK(after.rotation.y == before.rotation.y);
    CHECK(after.rotation.z == before.rotation.z);
    CHECK(after.rotation.w == before.rotation.w);
    CHECK(after.scale.x == before.scale.x);
    CHECK(after.scale.y == before.scale.y);
    CHECK(after.scale.z == before.scale.z);

    // Root dropped on the outliner unparent zone: -1 -> -1 is the same no-op.
    const iron::Transform rootBefore = s.entities[0].transform;
    CHECK(!iron::reparentKeepWorld(s, 0, -1));
    CHECK(s.entities[0].parentIndex == -1);
    CHECK(s.entities[0].transform.position.x == rootBefore.position.x);
    CHECK(s.entities[0].transform.rotation.w == rootBefore.rotation.w);
    CHECK(s.entities[0].transform.scale.x == rootBefore.scale.x);
}

static void test_reparent_between_two_parents_keeps_world() {
    iron::SceneFile s;
    iron::SceneEntity p1; p1.transform.position = {10, 0, 0};
    iron::SceneEntity p2; p2.transform.position = {0, 5, 0};
    iron::SceneEntity c;  c.transform.position  = {2, 0, 0}; c.parentIndex = 0;
    s.entities = {p1, p2, c};  // child's world pos = (12,0,0)
    const iron::Mat4 before = iron::worldMatrixOf(s, 2);
    CHECK(iron::reparentKeepWorld(s, 2, 1));
    CHECK(s.entities[2].parentIndex == 1);
    const iron::Mat4 after = iron::worldMatrixOf(s, 2);
    CHECK_NEAR(after.at(0,3), before.at(0,3));  // world X still 12
    CHECK_NEAR(after.at(1,3), before.at(1,3));  // world Y still 0
}

static void test_delete_subtree_remap() {
    // 0 root, 1 child-of-0, 2 grandchild-of-1, 3 unrelated root.
    iron::SceneFile s;
    iron::SceneEntity a; a.name="a";
    iron::SceneEntity b; b.name="b"; b.parentIndex=0;
    iron::SceneEntity c; c.name="c"; c.parentIndex=1;
    iron::SceneEntity d; d.name="d";
    s.entities = {a,b,c,d};

    auto map = iron::deleteSubtree(s, 1);     // removes b(1) and c(2)
    CHECK(map.size() == 4);
    CHECK(map[0] == 0);                        // a survives at 0
    CHECK(map[1] == -1);                       // b removed
    CHECK(map[2] == -1);                       // c removed
    CHECK(map[3] == 1);                        // d shifts down to 1
    CHECK(s.entities.size() == 2);
    CHECK(s.entities[0].name == "a");
    CHECK(s.entities[1].name == "d");
    CHECK(s.entities[1].parentIndex == -1);    // d still root
}

static void test_delete_subtree_remaps_surviving_parent_links() {
    // 0 root, 1 child-of-0, 2 sibling root with child 3.
    iron::SceneFile s;
    iron::SceneEntity a; a.name="a";
    iron::SceneEntity b; b.name="b"; b.parentIndex=0;
    iron::SceneEntity c; c.name="c";
    iron::SceneEntity e; e.name="e"; e.parentIndex=2;
    s.entities = {a,b,c,e};

    iron::deleteSubtree(s, 0);                 // remove a(0) and b(1)
    CHECK(s.entities.size() == 2);
    CHECK(s.entities[0].name == "c");
    CHECK(s.entities[1].name == "e");
    CHECK(s.entities[1].parentIndex == 0);     // e's parent c remapped 2 -> 0
}

static void test_delete_subtree_out_of_range_is_identity() {
    iron::SceneFile s = makeChain();           // 3 entities
    auto map = iron::deleteSubtree(s, 99);
    CHECK(map.size() == 3);
    CHECK(map[0] == 0);
    CHECK(map[1] == 1);
    CHECK(map[2] == 2);
    CHECK(s.entities.size() == 3);             // untouched
    CHECK(iron::duplicateSubtree(s, 99, [](const std::string& n){ return n; }) == -1);
}

static void test_delete_subtree_entire_scene() {
    iron::SceneFile s = makeChain();           // root(0)->mid(1)->leaf(2)
    auto map = iron::deleteSubtree(s, 0);      // removes everything
    CHECK(map.size() == 3);
    CHECK(map[0] == -1);
    CHECK(map[1] == -1);
    CHECK(map[2] == -1);
    CHECK(s.entities.empty());
}

static void test_duplicate_subtree() {
    iron::SceneFile s = makeChain();           // root(0)->mid(1)->leaf(2)
    auto uniq = [&](const std::string& base){ return base + "_copy"; };
    const int newRoot = iron::duplicateSubtree(s, 1, uniq);   // duplicate mid+leaf
    CHECK(s.entities.size() == 5);
    CHECK(newRoot == 3);                        // appended after the original 3
    CHECK(s.entities[3].name == "mid_copy");
    CHECK(s.entities[3].parentIndex == 0);      // new root attaches to source's parent (root)
    CHECK(s.entities[4].name == "leaf_copy");
    CHECK(s.entities[4].parentIndex == 3);      // internal link preserved (points at the copy)
}

int main() {
    test_transform_matrix_equals_inline_composition();
    test_world_matrix_three_deep_translation();
    test_world_matrix_with_rotation_and_scale();
    test_is_descendant();
    test_collect_subtree();
    test_cycle_guard_does_not_hang();
    test_reparent_preserves_world_position();
    test_reparent_to_root_keeps_world();
    test_reparent_rejections();
    test_reparent_onto_current_parent_is_noop();
    test_reparent_between_two_parents_keeps_world();
    test_delete_subtree_remap();
    test_delete_subtree_remaps_surviving_parent_links();
    test_delete_subtree_out_of_range_is_identity();
    test_delete_subtree_entire_scene();
    test_duplicate_subtree();
    return iron_test_result();
}
