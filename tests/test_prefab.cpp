#include "scene/Prefab.h"
#include "scene/SceneFormat.h"
#include "world/Transform.h"
#include "test_framework.h"

// root(0) -> mid(1) -> leaf(2), each offset +1 on X (local).
static iron::SceneFile makeChain() {
    iron::SceneFile s;
    iron::SceneEntity e0; e0.name = "root"; e0.transform.position = {1, 0, 0};
    iron::SceneEntity e1; e1.name = "mid";  e1.transform.position = {1, 0, 0}; e1.parentIndex = 0;
    iron::SceneEntity e2; e2.name = "leaf"; e2.transform.position = {1, 0, 0}; e2.parentIndex = 1;
    s.entities = {e0, e1, e2};
    return s;
}

static void test_extract_rebases_parent_indices() {
    iron::SceneFile s = makeChain();
    iron::Prefab p = iron::extractPrefab(s, 0);   // whole chain
    CHECK(p.entities.size() == 3);
    CHECK(p.entities[0].name == "root");
    CHECK(p.entities[0].parentIndex == -1);       // root
    CHECK(p.entities[1].parentIndex == 0);        // mid -> prefab-local root
    CHECK(p.entities[2].parentIndex == 1);        // leaf -> prefab-local mid
    CHECK_NEAR(p.entities[0].transform.position.x, 1.0f);
}

static void test_extract_partial_subtree() {
    iron::SceneFile s = makeChain();
    iron::Prefab p = iron::extractPrefab(s, 1);   // mid + leaf only
    CHECK(p.entities.size() == 2);
    CHECK(p.entities[0].name == "mid");
    CHECK(p.entities[0].parentIndex == -1);       // mid becomes the prefab root
    CHECK(p.entities[1].name == "leaf");
    CHECK(p.entities[1].parentIndex == 0);
}

static void test_extract_out_of_range_is_empty() {
    iron::SceneFile s = makeChain();
    CHECK(iron::extractPrefab(s, 9).entities.empty());
    CHECK(iron::extractPrefab(s, -1).entities.empty());
}

static void test_instantiate_reindexes_and_places_root() {
    iron::SceneFile s = makeChain();              // 3 entities at indices 0,1,2
    iron::Prefab p = iron::extractPrefab(s, 1);   // mid + leaf (2 entities)

    iron::Transform placement;
    placement.position = {9, 0, 0};
    auto uniq = [](const std::string& n){ return n + "_copy"; };

    const int newRoot = iron::instantiatePrefab(s, p, placement, uniq);
    CHECK(newRoot == 3);                           // appended after the original 3
    CHECK(s.entities.size() == 5);
    CHECK(s.entities[3].name == "mid_copy");
    CHECK(s.entities[3].parentIndex == -1);        // instantiated as a scene root
    CHECK(s.entities[4].name == "leaf_copy");
    CHECK(s.entities[4].parentIndex == 3);         // internal link -> the copy
    // Placement replaced the root transform.
    CHECK_NEAR(s.entities[3].transform.position.x, 9.0f);
    // Descendant kept its prefab-local offset (leaf was +1 on X).
    CHECK_NEAR(s.entities[4].transform.position.x, 1.0f);
}

static void test_instantiate_leaves_existing_indices_untouched() {
    iron::SceneFile s = makeChain();
    iron::Prefab p = iron::extractPrefab(s, 0);    // 3-entity prefab
    auto uniq = [](const std::string& n){ return n + "_2"; };
    iron::instantiatePrefab(s, p, iron::Transform{}, uniq);
    CHECK(s.entities.size() == 6);
    CHECK(s.entities[0].name == "root");           // originals unchanged
    CHECK(s.entities[1].parentIndex == 0);
    CHECK(s.entities[2].parentIndex == 1);
}

static void test_instantiate_empty_prefab_is_noop() {
    iron::SceneFile s = makeChain();
    iron::Prefab empty;
    auto uniq = [](const std::string& n){ return n; };
    CHECK(iron::instantiatePrefab(s, empty, iron::Transform{}, uniq) == -1);
    CHECK(s.entities.size() == 3);                  // unchanged
}

int main() {
    test_extract_rebases_parent_indices();
    test_extract_partial_subtree();
    test_extract_out_of_range_is_empty();
    test_instantiate_reindexes_and_places_root();
    test_instantiate_leaves_existing_indices_untouched();
    test_instantiate_empty_prefab_is_noop();
    return iron_test_result();
}
