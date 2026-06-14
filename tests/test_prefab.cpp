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

int main() {
    test_extract_rebases_parent_indices();
    test_extract_partial_subtree();
    test_extract_out_of_range_is_empty();
    return iron_test_result();
}
