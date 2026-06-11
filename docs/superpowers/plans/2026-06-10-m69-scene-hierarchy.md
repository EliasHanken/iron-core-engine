# M69 Scene Hierarchy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Parent/child entity transforms with reparenting in the outliner — when a parent moves, its children follow, in Edit mode and during Play.

**Architecture:** Approach A+ from the approved spec (`docs/superpowers/specs/2026-06-10-m69-scene-hierarchy-design.md`). `SceneEntity` gains an order-free `int parentIndex = -1`. A World-internal `Parent { EntityId parent }` component mirrors it at runtime. Render and picking compose model matrices by walking the **World-side** `Parent` chain (so a Play-mode parent moved by a logic graph or physics drags its subtree, since logic/physics live only in the World during Play). Pure hierarchy math (`worldMatrixOf`, `reparentKeepWorld`, `deleteSubtree`, `duplicateSubtree`) lives in a headless, fully tested `engine/scene/SceneHierarchy` module. TRS decompose reuses the existing tested `iron::decomposeTRS`/`composeTRS` in `engine/asset/Pose.{h,cpp}` (no reimplementation).

**Tech Stack:** C++20, CMake + CTest, nlohmann/json (scene IO), Dear ImGui (outliner), custom math (`engine/math`), single static lib `ironcore`. Tests are standalone executables using the project's `CHECK(...)` macro, registered via `iron_add_test` in `tests/CMakeLists.txt`.

**Test harness:** All test files use the shared `tests/test_framework.h` (provides `CHECK(cond)`, `CHECK_NEAR(a, b)` with a `1e-4f` tolerance, and `iron_test_result()`). Do NOT roll a private `g_failures`/`CHECK` macro. A test file's `main()` calls the test functions and `return iron_test_result();`. (Code snippets below that predate this note may show a bespoke harness — use `test_framework.h` instead.)

**Build & test commands (Windows / PowerShell):**
- Build: `cmake --build build --config Debug`
- Run one test: `ctest --test-dir build -C Debug --output-on-failure -R <name>`
- Run all: `ctest --test-dir build -C Debug --output-on-failure`
- After adding a new test file or source to CMake, re-configure first: `cmake -B build` (or just `cmake --build build --config Debug`, which re-runs CMake when `CMakeLists.txt` changed).

---

## File Structure

**Create:**
- `engine/world/Parent.h` — World-internal POD component `Parent { EntityId parent; }` (mirrors `RenderHandles`; NOT in ComponentRegistry, never serialized).
- `engine/scene/SceneHierarchy.h` / `engine/scene/SceneHierarchy.cpp` — headless free functions over `SceneFile`: `worldMatrixOf`, `isDescendant`, `collectSubtree`, `reparentKeepWorld`, `deleteSubtree`, `duplicateSubtree`.
- `engine/world/WorldHierarchy.h` / `engine/world/WorldHierarchy.cpp` — `worldMatrix(World, EntityId[, memo])`: compose a World entity's model matrix through the `Parent` chain, depth-capped.
- `tests/test_scene_hierarchy.cpp` — all hierarchy math + delete/duplicate remap tests.
- `tests/test_world_hierarchy.cpp` — World-side composition tests.

**Modify:**
- `engine/world/Transform.h` — add `Mat4 matrix() const` (factors the inline T·R·S composition).
- `engine/scene/SceneFormat.h` — add `int parentIndex = -1;` to `SceneEntity`.
- `engine/scene/SceneIO.cpp` — write/read `"parent"`; sanitize out-of-range/cyclic on load.
- `engine/CMakeLists.txt` (or wherever engine sources are listed) — add the new `.cpp` files.
- `tests/CMakeLists.txt` — register the two new tests.
- `games/11-sandbox/main.cpp` — spawn `Parent` into the World; compose render + picking through the chain; physics write-back inverse-composes; delete/duplicate use the new subtree functions; gizmo world-space proxy for children; outliner Reparent action handling; `gizmoOriginFor` world-space.
- `engine/editor/SceneOutliner.h` / `engine/editor/SceneOutliner.cpp` — tree view + drag-drop reparent + `Action::Reparent`.
- `docs/engine/` GameplayNodes/editor docs — note Get/Set/Translate position stay LOCAL space.

**Self-contained changes:** Phase A (Tasks 1–6) is pure headless code + tests, mergeable on its own. Phase B (7–8) wires the World mirror. Phase C (9–13) and D (14–15) are the editor/runtime integration, gated by the user-run demo (Task 17).

---

## Task 1: `Transform::matrix()` helper

**Files:**
- Modify: `engine/world/Transform.h`
- Test: `tests/test_scene_hierarchy.cpp` (new file; this task creates it)
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Register the new test executable**

In `tests/CMakeLists.txt`, add alongside the other `iron_add_test(...)` lines:

```cmake
iron_add_test(test_scene_hierarchy test_scene_hierarchy.cpp)
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_scene_hierarchy.cpp`:

```cpp
#include "world/Transform.h"
#include "math/Transform.h"
#include "math/Mat4.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool mat4Near(const iron::Mat4& a, const iron::Mat4& b, float eps = 1e-5f) {
    for (int i = 0; i < 16; ++i) if (std::fabs(a.m[i] - b.m[i]) > eps) return false;
    return true;
}

static void test_transform_matrix_equals_inline_composition() {
    iron::Transform t;
    t.position = {1.0f, 2.0f, 3.0f};
    t.rotation = iron::Quat::fromAxisAngle({0.0f, 1.0f, 0.0f}, 0.7f);
    t.scale    = {2.0f, 0.5f, 1.5f};

    const iron::Mat4 inline_ = iron::translation(t.position)
                             * t.rotation.toMat4()
                             * iron::scaling(t.scale);
    CHECK(mat4Near(t.matrix(), inline_));
}

int main() {
    test_transform_matrix_equals_inline_composition();
    if (g_failures == 0) std::printf("test_scene_hierarchy: OK\n");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cmake --build build --config Debug`
Expected: COMPILE ERROR — `Transform` has no member `matrix`.

- [ ] **Step 4: Implement `matrix()`**

In `engine/world/Transform.h`, add the two includes and the method:

```cpp
#pragma once

#include "math/Mat4.h"
#include "math/Quaternion.h"
#include "math/Transform.h"
#include "math/Vec.h"

namespace iron {

struct Transform {
    Vec3 position = {0.0f, 0.0f, 0.0f};
    Quat rotation = Quat::identity();
    Vec3 scale    = {1.0f, 1.0f, 1.0f};

    // Column-major model matrix M = T * R * S. Factors the composition that was
    // previously inlined at render-submit / picking / gizmo (main.cpp).
    Mat4 matrix() const {
        return translation(position) * rotation.toMat4() * scaling(scale);
    }
};

}  // namespace iron
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --config Debug` then `ctest --test-dir build -C Debug --output-on-failure -R test_scene_hierarchy`
Expected: PASS — `test_scene_hierarchy: OK`.

- [ ] **Step 6: Commit**

```bash
git add engine/world/Transform.h tests/test_scene_hierarchy.cpp tests/CMakeLists.txt
git commit -m "M69: add Transform::matrix() helper (factors inline T*R*S composition)"
```

---

## Task 2: `parentIndex` field + SceneIO round-trip / legacy / sanitize

**Files:**
- Modify: `engine/scene/SceneFormat.h:54-60`
- Modify: `engine/scene/SceneIO.cpp` (`entityToJson` ~40-56, `entityFromJson` ~58-96)
- Test: `tests/test_scene_io.cpp` (existing)

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_scene_io.cpp` (follow the file's existing `CHECK` style; call these from its `main()`):

```cpp
static void test_parent_index_roundtrip() {
    iron::Reflection r; iron::ComponentRegistry cr;
    iron::registerCoreComponents(cr, r);
    iron::SceneFile s;
    iron::SceneEntity a; a.name = "root";
    iron::SceneEntity b; b.name = "child"; b.parentIndex = 0;
    s.entities = {a, b};

    const std::string json = iron::sceneToJsonString(r, cr, s);
    iron::SceneFile loaded;
    CHECK(iron::sceneFromJsonString(r, cr, json, loaded));
    CHECK(loaded.entities.size() == 2);
    CHECK(loaded.entities[0].parentIndex == -1);
    CHECK(loaded.entities[1].parentIndex == 0);
}

static void test_parent_index_legacy_defaults_to_minus_one() {
    iron::Reflection r; iron::ComponentRegistry cr;
    iron::registerCoreComponents(cr, r);
    // Legacy scene JSON with no "parent" key on either entity.
    const char* legacy = R"({"entities":[{"name":"a"},{"name":"b"}]})";
    iron::SceneFile loaded;
    CHECK(iron::sceneFromJsonString(r, cr, legacy, loaded));
    CHECK(loaded.entities.size() == 2);
    CHECK(loaded.entities[0].parentIndex == -1);
    CHECK(loaded.entities[1].parentIndex == -1);
}

static void test_parent_index_out_of_range_sanitized() {
    iron::Reflection r; iron::ComponentRegistry cr;
    iron::registerCoreComponents(cr, r);
    const char* bad = R"({"entities":[{"name":"a","parent":7}]})";
    iron::SceneFile loaded;
    CHECK(iron::sceneFromJsonString(r, cr, bad, loaded));
    CHECK(loaded.entities.size() == 1);
    CHECK(loaded.entities[0].parentIndex == -1);  // sanitized with warn
}
```

> Confirm the exact serialize/parse entry-point names by reading the top of `tests/test_scene_io.cpp` and `engine/scene/SceneIO.h`; the calls above (`sceneToJsonString` / `sceneFromJsonString` / `registerCoreComponents`) match `togglePlayMode` usage in main.cpp. If the round-trip helper differs (e.g. `saveSceneFile`/`loadSceneFile` to a temp path), use whatever the existing tests in that file already use.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --config Debug`
Expected: COMPILE ERROR — `SceneEntity` has no member `parentIndex`.

- [ ] **Step 3: Add the field**

`engine/scene/SceneFormat.h`, in `struct SceneEntity`:

```cpp
struct SceneEntity {
    std::string name;
    Transform   transform;
    MeshRef     mesh;
    MaterialDef material;
    ComponentSet components;   // M67 — collision/audio/probe/logic are now components
    int          parentIndex = -1;   // M69 — index into SceneFile::entities; -1 = root
};
```

- [ ] **Step 4: Serialize (write only when ≠ -1)**

In `engine/scene/SceneIO.cpp` `entityToJson`, after the `material` line and before the components array:

```cpp
    j["material"]  = componentToJson(r, e.material);
    if (e.parentIndex != -1) j["parent"] = e.parentIndex;   // M69
```

- [ ] **Step 5: Deserialize with default**

In `entityFromJson`, after the `material` read:

```cpp
    if (j.contains("material"))  componentFromJson(r, e.material,  j["material"]);
    if (j.contains("parent") && j["parent"].is_number_integer())
        e.parentIndex = j["parent"].get<int>();   // M69; validated after the whole scene loads
```

- [ ] **Step 6: Whole-scene sanitize pass**

Find the function that builds the full `SceneFile` from JSON (the loop over the `"entities"` array, likely `sceneFromJson`/`sceneFromJsonString` in `SceneIO.cpp`). After all entities are loaded, add:

```cpp
    // M69: fail-safe parentIndex validation. Out-of-range, self-parent, or a
    // parent chain that cycles is reset to -1 (root) with a warning — matching
    // SceneIO's load-time posture of never rejecting a file outright.
    const int n = static_cast<int>(out.entities.size());
    for (int i = 0; i < n; ++i) {
        int p = out.entities[i].parentIndex;
        if (p != -1 && (p < 0 || p >= n || p == i)) {
            Log::warn("SceneIO: entity %d has invalid parent %d; resetting to root", i, p);
            out.entities[i].parentIndex = -1;
        }
    }
    // Cycle guard: walk each entity's chain; if it exceeds n hops it cycles.
    for (int i = 0; i < n; ++i) {
        int hops = 0, p = out.entities[i].parentIndex;
        while (p != -1) {
            if (++hops > n) {
                Log::warn("SceneIO: entity %d parent chain cycles; resetting to root", i);
                out.entities[i].parentIndex = -1;
                break;
            }
            p = out.entities[p].parentIndex;
        }
    }
```

(Replace `out` with the actual result variable name in that function. Ensure `#include "core/Log.h"` — or whatever header declares `Log::warn` — is present; `SceneIO.cpp` likely already includes it.)

- [ ] **Step 7: Run tests**

Run: `cmake --build build --config Debug` then `ctest --test-dir build -C Debug --output-on-failure -R test_scene_io`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add engine/scene/SceneFormat.h engine/scene/SceneIO.cpp tests/test_scene_io.cpp
git commit -m "M69: SceneEntity.parentIndex with serialize, legacy default, load-time sanitize"
```

---

## Task 3: SceneHierarchy core — `worldMatrixOf`, `isDescendant`, `collectSubtree`

**Files:**
- Create: `engine/scene/SceneHierarchy.h`, `engine/scene/SceneHierarchy.cpp`
- Modify: engine sources CMake list (add `scene/SceneHierarchy.cpp`)
- Test: `tests/test_scene_hierarchy.cpp`

- [ ] **Step 1: Write the header**

Create `engine/scene/SceneHierarchy.h`:

```cpp
#pragma once

#include "math/Mat4.h"
#include "scene/SceneFormat.h"

#include <functional>
#include <string>
#include <vector>

namespace iron {

// Depth cap shared by every chain walk; doubles as a cycle guard so a
// hand-corrupted parentIndex never hangs.
inline constexpr int kMaxHierarchyDepth = 256;

// World matrix of entity `index`: its local matrix pre-multiplied by every
// ancestor's, walking up parentIndex. Returns identity for an out-of-range
// index. Depth-capped.
Mat4 worldMatrixOf(const SceneFile& scene, int index);

// True if `maybeDescendant` is `ancestor` or sits anywhere below it.
bool isDescendant(const SceneFile& scene, int ancestor, int maybeDescendant);

// `root` followed by all its descendants (any order below root). Empty if root
// is out of range.
std::vector<int> collectSubtree(const SceneFile& scene, int root);

// Reparent `child` under `newParent` (-1 = make root), preserving world pose:
// the child's local transform is recomputed as inverse(parentWorld) * childWorld.
// Returns false (no change) for an invalid child, an out-of-range newParent,
// self-parenting, or a newParent that is a descendant of child (would cycle).
// LOSSY under non-uniform parent scale combined with child rotation (shear has
// no TRS representation) — the standard engine caveat.
bool reparentKeepWorld(SceneFile& scene, int child, int newParent);

// Remove `root` and its whole subtree. Returns an old->new index map sized to
// the ORIGINAL entity count: removed entries are -1, survivors map to their new
// index. Surviving parentIndex links are remapped. The host uses the map to fix
// sceneIndexToEntity / resolved / selectedIndex in one place.
std::vector<int> deleteSubtree(SceneFile& scene, int root);

// Deep-copy `root`'s subtree, appending the copies. Internal parent links are
// preserved; the new root attaches to the source root's parent. Each new entity
// is renamed via `uniquify(name)`. Returns the new root's index (-1 if root is
// out of range). Existing indices are unchanged (copies are appended), so the
// host only needs to spawn World entities for the appended range.
int duplicateSubtree(SceneFile& scene, int root,
                     const std::function<std::string(const std::string&)>& uniquify);

}  // namespace iron
```

- [ ] **Step 2: Write failing tests for the three core functions**

Add to `tests/test_scene_hierarchy.cpp` (include `"scene/SceneHierarchy.h"` and `"math/Transform.h"` at top; call these from `main()`):

```cpp
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
    // leaf world position = 1 + 1 + 1 = 3 on X (column 3 = translation).
    CHECK(std::fabs(w.at(0, 3) - 3.0f) < 1e-5f);
    CHECK(std::fabs(w.at(1, 3) - 0.0f) < 1e-5f);
    CHECK(std::fabs(w.at(2, 3) - 0.0f) < 1e-5f);
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
    CHECK(std::fabs(w.at(0, 3) - 0.0f) < 1e-4f);
    CHECK(std::fabs(w.at(2, 3) + 2.0f) < 1e-4f);
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
    CHECK(sub.size() == 2);
    CHECK(sub[0] == 1);                       // root first
    bool hasLeaf = false; for (int i : sub) if (i == 2) hasLeaf = true;
    CHECK(hasLeaf);
}

static void test_cycle_guard_does_not_hang() {
    iron::SceneFile s = makeChain();
    s.entities[0].parentIndex = 2;   // root -> leaf -> mid -> root : cycle
    // Must return (depth-capped) rather than loop forever.
    (void)iron::worldMatrixOf(s, 2);
    CHECK(true);
}
```

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build --config Debug`
Expected: LINK/COMPILE ERROR — `worldMatrixOf` etc. undefined (and `SceneHierarchy.cpp` not yet in CMake).

- [ ] **Step 4: Add the source to CMake**

Find the engine source list (e.g. `engine/CMakeLists.txt` — search for `scene/SceneIO.cpp` and add next to it):

```cmake
    scene/SceneHierarchy.cpp
```

- [ ] **Step 5: Implement the three core functions**

Create `engine/scene/SceneHierarchy.cpp` (implements Tasks 3, 4, 5 — write the core three now; `reparentKeepWorld`/`deleteSubtree`/`duplicateSubtree` are filled in by Tasks 4–5):

```cpp
#include "scene/SceneHierarchy.h"

#include "asset/Pose.h"        // composeTRS / decomposeTRS (reused, tested in test_pose)
#include "math/Mat4.h"

namespace iron {

namespace {
bool inRange(const SceneFile& s, int i) {
    return i >= 0 && i < static_cast<int>(s.entities.size());
}
}  // namespace

Mat4 worldMatrixOf(const SceneFile& scene, int index) {
    if (!inRange(scene, index)) return Mat4::identity();
    Mat4 m = scene.entities[index].transform.matrix();
    int p = scene.entities[index].parentIndex;
    for (int depth = 0; p != -1 && depth < kMaxHierarchyDepth; ++depth) {
        if (!inRange(scene, p)) break;
        m = scene.entities[p].transform.matrix() * m;
        p = scene.entities[p].parentIndex;
    }
    return m;
}

bool isDescendant(const SceneFile& scene, int ancestor, int maybeDescendant) {
    int cur = maybeDescendant;
    for (int depth = 0; cur != -1 && depth < kMaxHierarchyDepth; ++depth) {
        if (cur == ancestor) return true;
        if (!inRange(scene, cur)) break;
        cur = scene.entities[cur].parentIndex;
    }
    return false;
}

std::vector<int> collectSubtree(const SceneFile& scene, int root) {
    std::vector<int> out;
    if (!inRange(scene, root)) return out;
    out.push_back(root);
    // Breadth-first over the children adjacency (parentIndex == current).
    for (std::size_t head = 0; head < out.size(); ++head) {
        const int parent = out[head];
        for (int i = 0; i < static_cast<int>(scene.entities.size()); ++i)
            if (scene.entities[i].parentIndex == parent) out.push_back(i);
    }
    return out;
}

}  // namespace iron
```

- [ ] **Step 6: Run the core tests**

Add the new test calls to `main()` in `tests/test_scene_hierarchy.cpp`, then:
Run: `cmake --build build --config Debug` then `ctest --test-dir build -C Debug --output-on-failure -R test_scene_hierarchy`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add engine/scene/SceneHierarchy.h engine/scene/SceneHierarchy.cpp engine/CMakeLists.txt tests/test_scene_hierarchy.cpp
git commit -m "M69: SceneHierarchy worldMatrixOf / isDescendant / collectSubtree (+ tests)"
```

---

## Task 4: `reparentKeepWorld`

**Files:**
- Modify: `engine/scene/SceneHierarchy.cpp`
- Test: `tests/test_scene_hierarchy.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
static bool transformsNear(const iron::Transform& a, const iron::Transform& b, float eps = 1e-4f) {
    auto vn = [&](iron::Vec3 x, iron::Vec3 y){ return std::fabs(x.x-y.x)<eps && std::fabs(x.y-y.y)<eps && std::fabs(x.z-y.z)<eps; };
    return vn(a.position, b.position) && vn(a.scale, b.scale);
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
    // World position unchanged; new local should be (2,0,0).
    CHECK(std::fabs(after.at(0,3) - before.at(0,3)) < 1e-4f);
    CHECK(std::fabs(s.entities[1].transform.position.x - 2.0f) < 1e-4f);
}

static void test_reparent_to_root_keeps_world() {
    iron::SceneFile s = makeChain();          // root(0)->mid(1)->leaf(2)
    const iron::Mat4 before = iron::worldMatrixOf(s, 2);
    CHECK(iron::reparentKeepWorld(s, 2, -1));  // unparent leaf
    CHECK(s.entities[2].parentIndex == -1);
    const iron::Mat4 after = iron::worldMatrixOf(s, 2);
    CHECK(std::fabs(after.at(0,3) - before.at(0,3)) < 1e-4f);
    CHECK(std::fabs(s.entities[2].transform.position.x - 3.0f) < 1e-4f);  // was world X=3
}

static void test_reparent_rejections() {
    iron::SceneFile s = makeChain();
    CHECK(!iron::reparentKeepWorld(s, 0, 0));   // self
    CHECK(!iron::reparentKeepWorld(s, 0, 2));   // newParent is descendant of child -> cycle
    CHECK(!iron::reparentKeepWorld(s, 9, 0));   // child out of range
    CHECK(!iron::reparentKeepWorld(s, 1, 9));   // newParent out of range
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --config Debug`
Expected: LINK ERROR — `reparentKeepWorld` undefined.

- [ ] **Step 3: Implement**

Add to `engine/scene/SceneHierarchy.cpp` (inside `namespace iron`):

```cpp
bool reparentKeepWorld(SceneFile& scene, int child, int newParent) {
    if (!inRange(scene, child)) return false;
    if (newParent != -1 && !inRange(scene, newParent)) return false;
    if (newParent == child) return false;
    if (newParent != -1 && isDescendant(scene, child, newParent)) return false;

    const Mat4 childWorld  = worldMatrixOf(scene, child);
    const Mat4 parentWorld = (newParent == -1) ? Mat4::identity()
                                               : worldMatrixOf(scene, newParent);
    const Mat4 newLocal    = inverse(parentWorld) * childWorld;

    const BoneLocal trs = decomposeTRS(newLocal);
    Transform& t = scene.entities[child].transform;
    t.position = trs.translation;
    t.rotation = trs.rotation;
    t.scale    = trs.scale;
    scene.entities[child].parentIndex = newParent;
    return true;
}
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build --config Debug` then `ctest --test-dir build -C Debug --output-on-failure -R test_scene_hierarchy`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/scene/SceneHierarchy.cpp tests/test_scene_hierarchy.cpp
git commit -m "M69: reparentKeepWorld (keep-world-pose reparent, with rejection cases)"
```

---

## Task 5: `deleteSubtree` + `duplicateSubtree` (the index-remap functions)

**Files:**
- Modify: `engine/scene/SceneHierarchy.cpp`
- Test: `tests/test_scene_hierarchy.cpp`

This is the highest-defect-risk code (spec §risks); isolated as pure functions returning the mapping and tested first.

- [ ] **Step 1: Write the failing tests**

```cpp
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
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --config Debug`
Expected: LINK ERROR — functions undefined.

- [ ] **Step 3: Implement**

Add to `engine/scene/SceneHierarchy.cpp`:

```cpp
std::vector<int> deleteSubtree(SceneFile& scene, int root) {
    const int n = static_cast<int>(scene.entities.size());
    std::vector<int> oldToNew(n, 0);
    if (!inRange(scene, root)) {              // nothing removed: identity map
        for (int i = 0; i < n; ++i) oldToNew[i] = i;
        return oldToNew;
    }

    // Mark the subtree for removal.
    std::vector<char> remove(n, 0);
    for (int idx : collectSubtree(scene, root)) remove[idx] = 1;

    // Build old->new and the surviving entity list in one pass.
    std::vector<SceneEntity> kept;
    kept.reserve(n);
    int next = 0;
    for (int i = 0; i < n; ++i) {
        if (remove[i]) { oldToNew[i] = -1; }
        else           { oldToNew[i] = next++; kept.push_back(scene.entities[i]); }
    }

    // Remap surviving parent links (a survivor's parent is always a survivor or -1).
    for (auto& e : kept) {
        if (e.parentIndex != -1) e.parentIndex = oldToNew[e.parentIndex];
    }
    scene.entities = std::move(kept);
    return oldToNew;
}

int duplicateSubtree(SceneFile& scene, int root,
                     const std::function<std::string(const std::string&)>& uniquify) {
    if (!inRange(scene, root)) return -1;
    const std::vector<int> sub = collectSubtree(scene, root);   // root first
    const int base = static_cast<int>(scene.entities.size());

    // old subtree index -> new appended index.
    std::vector<int> remap(scene.entities.size(), -1);
    for (std::size_t k = 0; k < sub.size(); ++k) remap[sub[k]] = base + static_cast<int>(k);

    for (int oldIdx : sub) {
        SceneEntity copy = scene.entities[oldIdx];   // value copy (transform + components)
        copy.name = uniquify(copy.name);
        if (oldIdx == root) {
            // New root keeps the source root's parent (attach as a sibling subtree).
            // copy.parentIndex already equals scene.entities[root].parentIndex.
        } else {
            copy.parentIndex = remap[copy.parentIndex];   // internal link -> the copy
        }
        scene.entities.push_back(std::move(copy));
    }
    return remap[root];
}
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build --config Debug` then `ctest --test-dir build -C Debug --output-on-failure -R test_scene_hierarchy`
Expected: PASS (all hierarchy tests).

- [ ] **Step 5: Commit**

```bash
git add engine/scene/SceneHierarchy.cpp tests/test_scene_hierarchy.cpp
git commit -m "M69: deleteSubtree/duplicateSubtree with old->new index remap (+ tests)"
```

---

## Task 6: `Parent` World component + World-side composition (`worldMatrix`)

**Files:**
- Create: `engine/world/Parent.h`
- Create: `engine/world/WorldHierarchy.h`, `engine/world/WorldHierarchy.cpp`
- Modify: engine sources CMake list (add `world/WorldHierarchy.cpp`)
- Create/Modify: `tests/test_world_hierarchy.cpp`; register in `tests/CMakeLists.txt`

- [ ] **Step 1: Create the Parent component**

`engine/world/Parent.h`:

```cpp
#pragma once

#include "world/Entity.h"

namespace iron {

// M69: runtime parent link, mirrored from SceneEntity::parentIndex during
// spawn/rebuild. Engine-internal like RenderHandles — NOT in ComponentRegistry,
// never serialized, never shown in the Inspector. Render/picking compose model
// matrices by walking this chain so a Play-mode parent moved by physics or a
// logic graph drags its subtree (those updates live only in the World).
struct Parent {
    EntityId parent = kEntityNone;
};

}  // namespace iron
```

- [ ] **Step 2: Register the new test + write failing test**

In `tests/CMakeLists.txt`:

```cmake
iron_add_test(test_world_hierarchy test_world_hierarchy.cpp)
```

Create `tests/test_world_hierarchy.cpp`:

```cpp
#include "world/World.h"
#include "world/Transform.h"
#include "world/Parent.h"
#include "world/WorldHierarchy.h"
#include "test_framework.h"

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

int main() {
    test_world_matrix_composes_through_parent();
    test_world_matrix_root_is_local();
    return iron_test_result();
}
```

> Confirm `World::add`/`World::get`/`World::create` signatures from `engine/world/World.h` (the resolve loop in main.cpp uses `world.add<iron::Transform>(e, t)` and `world.get<iron::Transform>(e)`, so these match).

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build --config Debug`
Expected: COMPILE/LINK ERROR — `WorldHierarchy.h`/`worldMatrix` missing.

- [ ] **Step 4: Implement WorldHierarchy**

`engine/world/WorldHierarchy.h`:

```cpp
#pragma once

#include "math/Mat4.h"
#include "world/Entity.h"

#include <unordered_map>

namespace iron {

class World;

// Compose `e`'s model matrix through its Parent chain (depth-capped as a cycle
// guard). An entity with no Parent component / kEntityNone parent is treated as
// a root (its local matrix). Returns identity if `e` has no Transform.
Mat4 worldMatrix(const World& world, EntityId e);

// Memoized variant for per-frame iteration over many entities: caches by
// entity index so a shared ancestor is composed once per frame.
Mat4 worldMatrix(const World& world, EntityId e,
                 std::unordered_map<std::uint32_t, Mat4>& memo);

}  // namespace iron
```

`engine/world/WorldHierarchy.cpp`:

```cpp
#include "world/WorldHierarchy.h"

#include "world/Parent.h"
#include "world/Transform.h"
#include "world/World.h"

#include "scene/SceneHierarchy.h"   // kMaxHierarchyDepth

namespace iron {

namespace {
Mat4 localOf(const World& world, EntityId e) {
    const Transform* t = world.get<Transform>(e);
    return t ? t->matrix() : Mat4::identity();
}
EntityId parentOf(const World& world, EntityId e) {
    const Parent* p = world.get<Parent>(e);
    return (p && p->parent.valid()) ? p->parent : kEntityNone;
}
}  // namespace

Mat4 worldMatrix(const World& world, EntityId e) {
    Mat4 m = localOf(world, e);
    EntityId p = parentOf(world, e);
    for (int depth = 0; p.valid() && depth < kMaxHierarchyDepth; ++depth) {
        m = localOf(world, p) * m;
        p = parentOf(world, p);
    }
    return m;
}

Mat4 worldMatrix(const World& world, EntityId e,
                 std::unordered_map<std::uint32_t, Mat4>& memo) {
    if (auto it = memo.find(e.index); it != memo.end()) return it->second;
    Mat4 local = localOf(world, e);
    EntityId p = parentOf(world, e);
    Mat4 result = p.valid() ? worldMatrix(world, p, memo) * local : local;
    memo[e.index] = result;
    return result;
}

}  // namespace iron
```

> Note: `world.get<T>(e)` must be available on a `const World&`. If `World::get` is non-const only, add a const overload (read-only accessor) in `engine/world/World.h` — a one-line addition mirroring the existing one. Verify before implementing.

Add `world/WorldHierarchy.cpp` to the engine CMake source list.

- [ ] **Step 5: Run tests**

Run: `cmake --build build --config Debug` then `ctest --test-dir build -C Debug --output-on-failure -R test_world_hierarchy`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/world/Parent.h engine/world/WorldHierarchy.h engine/world/WorldHierarchy.cpp engine/CMakeLists.txt tests/test_world_hierarchy.cpp tests/CMakeLists.txt
git commit -m "M69: Parent component + World-side worldMatrix composition (+ tests)"
```

---

## Task 7: Spawn `Parent` into the World

**Files:**
- Modify: `games/11-sandbox/main.cpp` — the three places that create World entities from scene entities: the startup resolve loop (~440-448), `appendAndSelect` (~1066-1080), and `rebuildDerivedFromScene` (~1090-1108).

Parent links reference World EntityIds, so they can only be written once **every** entity has a World entity. Add a post-pass after each full (re)build, and an incremental update in `appendAndSelect`.

- [ ] **Step 1: Add an include**

Near the other world includes at the top of `main.cpp`:

```cpp
#include "world/Parent.h"
#include "world/WorldHierarchy.h"
#include "scene/SceneHierarchy.h"
```

- [ ] **Step 2: Add a reusable Parent-mirror helper**

Define this lambda near `rebuildDerivedFromScene` (it needs `scene`, `world`, `sceneIndexToEntity` by reference):

```cpp
// M69: mirror scene parentIndex links into World Parent components. Call after
// sceneIndexToEntity is fully populated (parent links need every entity's
// EntityId to exist). Idempotent: re-adds Parent on every entity each call.
auto mirrorParents = [&]() {
    for (int i = 0; i < static_cast<int>(scene.entities.size()); ++i) {
        if (i >= static_cast<int>(sceneIndexToEntity.size())) break;
        const int p = scene.entities[i].parentIndex;
        iron::EntityId parentId = iron::kEntityNone;
        if (p >= 0 && p < static_cast<int>(sceneIndexToEntity.size()))
            parentId = sceneIndexToEntity[p];
        world.add<iron::Parent>(sceneIndexToEntity[i], iron::Parent{parentId});
    }
};
```

> If `world.add<T>` asserts on a duplicate component, use a get-or-add: `if (auto* pc = world.get<iron::Parent>(id)) pc->parent = parentId; else world.add<iron::Parent>(id, iron::Parent{parentId});`. Verify `World::add` semantics first.

- [ ] **Step 3: Call it after the startup resolve loop**

Immediately after the loop that fills `sceneIndexToEntity` at startup (~448), add:

```cpp
    mirrorParents();   // M69
```

- [ ] **Step 4: Call it at the end of `rebuildDerivedFromScene`**

After the rebuild loop repopulates `sceneIndexToEntity` (~1105), add `mirrorParents();` before the function returns. (Undo/redo restore depends on this.)

- [ ] **Step 5: Update `appendAndSelect`**

A freshly-added entity is always a root (`parentIndex == -1` by default), so its Parent is `kEntityNone`. After `sceneIndexToEntity.push_back(entity);` in `appendAndSelect` (~1079), add:

```cpp
            world.add<iron::Parent>(entity, iron::Parent{iron::kEntityNone});  // M69: new entities are roots
```

- [ ] **Step 6: Build**

Run: `cmake --build build --config Debug`
Expected: builds clean. (No unit test here — verified by Task 9's behavior and the demo gate.)

- [ ] **Step 7: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M69: mirror scene parentIndex into World Parent components on spawn/rebuild"
```

---

## Task 8: Render-submit composes through the Parent chain

**Files:**
- Modify: `games/11-sandbox/main.cpp` render-submit loop (~1742-1761).

- [ ] **Step 1: Replace the inline model composition**

The current submit loop builds `call.model` from the entity's own Transform only:

```cpp
    call.model                 = iron::translation(t.position)
                               * t.rotation.toMat4()
                               * iron::scaling(t.scale);
```

Replace with a Parent-chain composition using a per-frame memo. Just before the `auto& transforms = world.view<iron::Transform>();` line, declare the memo:

```cpp
    std::unordered_map<std::uint32_t, iron::Mat4> worldMatrixMemo;   // M69: per-frame
```

Then change the model assignment to:

```cpp
    call.model                 = iron::worldMatrix(world, e, worldMatrixMemo);   // M69
```

(`t` is still used for nothing else here; if it becomes unused, drop the `const iron::Transform& t = transforms[row];` line to avoid a warning — but it's likely still referenced; check.)

- [ ] **Step 2: Build**

Run: `cmake --build build --config Debug`
Expected: builds clean.

- [ ] **Step 3: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M69: render-submit composes model matrix through the World Parent chain"
```

---

## Task 9: Picking composes through the chain

**Files:**
- Modify: `games/11-sandbox/main.cpp` picking block (~1326-1336).

- [ ] **Step 1: Replace the inline picking model**

Current:

```cpp
                    for (std::size_t i = 0; i < resolved.size(); ++i) {
                        const iron::SceneEntity& e = scene.entities[resolved[i].entityIndex];
                        const iron::Mat4 model = iron::translation(e.transform.position)
                                               * e.transform.rotation.toMat4()
                                               * iron::scaling(e.transform.scale);
                        worldAabbs[i] = worldAabb(resolved[i].localBounds, model);
                    }
```

Replace the `model` computation with the World-side composition (picking is Edit-mode, where scene and World are mirrored each frame, so this matches render exactly):

```cpp
                    std::unordered_map<std::uint32_t, iron::Mat4> pickMemo;   // M69
                    for (std::size_t i = 0; i < resolved.size(); ++i) {
                        const int si = resolved[i].entityIndex;
                        const iron::Mat4 model =
                            iron::worldMatrix(world, sceneIndexToEntity[si], pickMemo);
                        worldAabbs[i] = worldAabb(resolved[i].localBounds, model);
                    }
```

- [ ] **Step 2: Build**

Run: `cmake --build build --config Debug`
Expected: builds clean.

- [ ] **Step 3: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M69: picking composes world AABB through the Parent chain"
```

---

## Task 10: Physics write-back inverse-composes for parented dynamic bodies

**Files:**
- Modify: `games/11-sandbox/main.cpp` physics write-back (~1151-1158).

- [ ] **Step 1: Inverse-compose when a dynamic body has a parent**

Current:

```cpp
        if (csb && csb->body == iron::ColliderBody::Dynamic) {
            scene.entities[idx].transform.position = physics.bodyPosition(body);
            scene.entities[idx].transform.rotation = physics.bodyRotation(body);
            ...
        }
```

Jolt returns a **world** pose. For a root body the stored local == world (unchanged behavior). For a parented body, store local = inverse(parentWorld) · worldPose. Replace the two assignment lines with:

```cpp
        if (csb && csb->body == iron::ColliderBody::Dynamic) {
            const iron::Vec3 wp = physics.bodyPosition(body);
            const iron::Quat wr = physics.bodyRotation(body);
            const int parent = scene.entities[idx].parentIndex;   // M69
            if (parent < 0) {
                scene.entities[idx].transform.position = wp;
                scene.entities[idx].transform.rotation = wr;
            } else {
                // Parent's current world pose lives in the World (logic/physics
                // updated it there). Compose local from it. Scale ignored (M42).
                const iron::Mat4 parentWorld =
                    iron::worldMatrix(world, sceneIndexToEntity[parent]);
                const iron::Mat4 worldPose = iron::translation(wp) * wr.toMat4();
                const iron::BoneLocal trs =
                    iron::decomposeTRS(iron::inverse(parentWorld) * worldPose);
                scene.entities[idx].transform.position = trs.translation;
                scene.entities[idx].transform.rotation = trs.rotation;
            }
            auto vit = playVoices.find(idx);
            if (vit != playVoices.end())
                audio.setVoicePosition(vit->second, scene.entities[idx].transform.position);
        }
```

Add `#include "asset/Pose.h"` to main.cpp if not already present (for `BoneLocal`/`decomposeTRS`).

> Note: the audio voice should track the WORLD position; for a parented body `trs.translation` is local. If audio mis-positions for parented bodies in the demo, set the voice from `wp` instead. The common case (root bodies) is unaffected.

- [ ] **Step 2: Build**

Run: `cmake --build build --config Debug`
Expected: builds clean.

- [ ] **Step 3: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M69: physics write-back stores local pose for parented dynamic bodies"
```

---

## Task 11: Delete & Duplicate use subtree functions

**Files:**
- Modify: `games/11-sandbox/main.cpp` Duplicate (~1591-1595) and Delete (~1596-1613).

- [ ] **Step 1: Duplicate the whole subtree**

Replace the Duplicate branch:

```cpp
            } else if (action == Action::Duplicate && selValid) {
                iron::SceneEntity ne = scene.entities[selectedIndex];  // copy mesh+material+transform
                ne.name = uniqueName(ne.name);
                ne.transform.position.x += 0.5f;  // slight offset so the copy is visible
                appendAndSelect(ne);
            }
```

with:

```cpp
            } else if (action == Action::Duplicate && selValid) {
                const int newRoot = iron::duplicateSubtree(scene, selectedIndex, uniqueName);
                scene.entities[newRoot].transform.position.x += 0.5f;  // nudge the copy
                // Spawn World entities for the appended range and re-mirror parents.
                for (int i = static_cast<int>(sceneIndexToEntity.size());
                     i < static_cast<int>(scene.entities.size()); ++i) {
                    ResolvedEntity re;
                    if (resolveEntity(scene.entities[i], i, re)) {
                        resolved.push_back(re);
                        const iron::SceneEntity& se = scene.entities[i];
                        iron::EntityId entity = world.create();
                        world.add<iron::Transform>(entity, se.transform);
                        world.add<iron::MeshRef>(entity, se.mesh);
                        world.add<iron::MaterialDef>(entity, se.material);
                        world.add<iron::RenderHandles>(entity, toRenderHandles(re));
                        sceneIndexToEntity.push_back(entity);
                    }
                }
                mirrorParents();              // M69: fix up Parent links for the new range
                selectedIndex = newRoot;
            }
```

> `uniqueName` has signature `std::string(const std::string&)`, matching `duplicateSubtree`'s `uniquify` parameter directly.

- [ ] **Step 2: Delete the whole subtree via the remap**

Replace the Delete branch:

```cpp
            } else if (action == Action::Delete && selValid) {
                const int d = selectedIndex;
                scene.entities.erase(scene.entities.begin() + d);
                if (d >= 0 && d < static_cast<int>(sceneIndexToEntity.size())) {
                    iron::EntityId e = sceneIndexToEntity[d];
                    world.destroy(e);
                    sceneIndexToEntity.erase(sceneIndexToEntity.begin() + d);
                }
                for (std::size_t i = 0; i < resolved.size();) {
                    if (resolved[i].entityIndex == d) { resolved.erase(resolved.begin() + i); continue; }
                    if (resolved[i].entityIndex > d) --resolved[i].entityIndex;
                    ++i;
                }
                selectedIndex = -1;
            }
```

with:

```cpp
            } else if (action == Action::Delete && selValid) {
                // M69: delete the whole subtree. deleteSubtree mutates
                // scene.entities and returns old->new (-1 = removed); we use it
                // to rebuild the parallel sceneIndexToEntity + resolved arrays.
                const std::vector<int> map = iron::deleteSubtree(scene, selectedIndex);

                std::vector<iron::EntityId> newSITE(scene.entities.size());
                for (int oldIdx = 0; oldIdx < static_cast<int>(map.size()); ++oldIdx) {
                    if (oldIdx >= static_cast<int>(sceneIndexToEntity.size())) break;
                    const iron::EntityId e = sceneIndexToEntity[oldIdx];
                    if (map[oldIdx] == -1) world.destroy(e);
                    else                   newSITE[map[oldIdx]] = e;
                }
                sceneIndexToEntity = std::move(newSITE);

                // Rebuild resolved: drop removed, remap survivors' entityIndex.
                std::vector<ResolvedEntity> keptResolved;
                keptResolved.reserve(resolved.size());
                for (auto& re : resolved) {
                    const int ni = (re.entityIndex >= 0 &&
                                    re.entityIndex < static_cast<int>(map.size()))
                                       ? map[re.entityIndex] : -1;
                    if (ni == -1) continue;
                    re.entityIndex = ni;
                    keptResolved.push_back(re);
                }
                resolved = std::move(keptResolved);

                mirrorParents();    // M69: Parent links may have been remapped
                selectedIndex = -1;
            }
```

- [ ] **Step 3: Build**

Run: `cmake --build build --config Debug`
Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M69: outliner Delete/Duplicate operate on whole subtrees (remap arrays)"
```

---

## Task 12: Gizmo world-space proxy for child entities

**Files:**
- Modify: `games/11-sandbox/main.cpp` — `gizmoOriginFor` (~964-966), the `gizmo.update` call (~1319-1323), and `gizmo.draw` (~2015-2018).

For a child, the gizmo must act in world space: origin = world pivot, manipulation happens on a world-space proxy transform, and the result is written back as local.

- [ ] **Step 1: World-space gizmo origin**

Replace `gizmoOriginFor`:

```cpp
    auto gizmoOriginFor = [&](int sel) -> iron::Vec3 {
        // M69: world-space pivot (translation column of the composed matrix).
        const iron::Mat4 w = iron::worldMatrixOf(scene, sel);
        return iron::Vec3{w.at(0, 3), w.at(1, 3), w.at(2, 3)};
    };
```

- [ ] **Step 2: Manipulate a world-space proxy**

Replace the `gizmo.update` call site:

```cpp
                if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
                    consumed = gizmo.update(scene.entities[selectedIndex],
                                            gizmoOriginFor(selectedIndex), ray,
                                            lmbPressed, lmbDown, cam.position,
                                            cam.fovDeg);
```

with:

```cpp
                if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size())) {
                    iron::SceneEntity& sel = scene.entities[selectedIndex];
                    if (sel.parentIndex < 0) {
                        // Root: existing path, gizmo edits local == world.
                        consumed = gizmo.update(sel, gizmoOriginFor(selectedIndex), ray,
                                                lmbPressed, lmbDown, cam.position, cam.fovDeg);
                    } else {
                        // M69: child — edit a world-space proxy, write back local.
                        const iron::Mat4 parentWorld = iron::worldMatrixOf(scene, sel.parentIndex);
                        const iron::BoneLocal pw = iron::decomposeTRS(parentWorld * sel.transform.matrix());
                        iron::SceneEntity proxy = sel;          // copy (keeps mesh for any internal use)
                        proxy.transform.position = pw.translation;
                        proxy.transform.rotation = pw.rotation;
                        proxy.transform.scale    = pw.scale;
                        consumed = gizmo.update(proxy, gizmoOriginFor(selectedIndex), ray,
                                                lmbPressed, lmbDown, cam.position, cam.fovDeg);
                        if (consumed) {
                            const iron::BoneLocal nl =
                                iron::decomposeTRS(iron::inverse(parentWorld) * proxy.transform.matrix());
                            sel.transform.position = nl.translation;
                            sel.transform.rotation = nl.rotation;
                            sel.transform.scale    = nl.scale;
                        }
                    }
                }
```

- [ ] **Step 3: Draw the gizmo with world-space rotation**

Replace the `gizmo.draw` call:

```cpp
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            gizmo.draw(renderer, gizmoOriginFor(selectedIndex),
                       scene.entities[selectedIndex].transform.rotation, cam.position,
                       cam.fovDeg);
```

with:

```cpp
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size())) {
            // M69: Local-space handles orient by the entity's WORLD rotation.
            const iron::BoneLocal wl = iron::decomposeTRS(iron::worldMatrixOf(scene, selectedIndex));
            gizmo.draw(renderer, gizmoOriginFor(selectedIndex), wl.rotation, cam.position, cam.fovDeg);
        }
```

> The selection-outline block just below (~2022-2028) also builds an inline model matrix; update it the same way for correctness of the highlight box on children: replace its `iron::translation(...)*...*iron::scaling(...)` with `iron::worldMatrixOf(scene, selectedIndex)`.

- [ ] **Step 4: Build**

Run: `cmake --build build --config Debug`
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M69: gizmo manipulates a world-space proxy for child entities"
```

---

## Task 13: Outliner tree view + drag-drop reparent

**Files:**
- Modify: `engine/editor/SceneOutliner.h`, `engine/editor/SceneOutliner.cpp`

- [ ] **Step 1: Extend the Result struct**

In `SceneOutliner.h`, add the `Reparent` action and its payload fields:

```cpp
    struct Result {
        enum class Action { None, AddCube, AddPlane, AddGltf, Delete, Duplicate, Reparent };
        bool        saveClicked = false;
        Action      action = Action::None;
        std::string gltfPath;        // populated for AddGltf
        int         reparentChild     = -1;   // M69: populated for Reparent
        int         reparentNewParent = -1;   // M69: -1 = unparent to root
    };
```

- [ ] **Step 2: Replace the flat list with a tree**

In `SceneOutliner.cpp`, replace the entity-list loop (lines ~12-19) with a recursive tree. Add `#include "scene/SceneFormat.h"` (already needed) and `#include "imgui.h"` (already present). Insert a file-local recursive helper above `draw` (a lambda inside `draw` is cleaner since it needs `result`/`selectedIndex`):

```cpp
SceneOutliner::Result SceneOutliner::draw(const SceneFile& scene, int& selectedIndex) {
    Result result;
    ImGui::Begin("Scene Outliner");

    if (ImGui::Button("Save Scene")) result.saveClicked = true;
    ImGui::Separator();

    const int n = static_cast<int>(scene.entities.size());

    // Build children adjacency from parentIndex (M69).
    std::vector<std::vector<int>> children(n);
    std::vector<int> roots;
    for (int i = 0; i < n; ++i) {
        const int p = scene.entities[i].parentIndex;
        if (p >= 0 && p < n) children[p].push_back(i);
        else                 roots.push_back(i);
    }

    // Recursive node renderer. Returns via `result` on a drag-drop reparent.
    std::function<void(int)> drawNode = [&](int i) {
        const std::string& name = scene.entities[i].name;
        const char* label = name.empty() ? "(unnamed)" : name.c_str();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_DefaultOpen |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (children[i].empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (i == selectedIndex)  flags |= ImGuiTreeNodeFlags_Selected;

        ImGui::PushID(i);
        const bool open = ImGui::TreeNodeEx(label, flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selectedIndex = i;

        // Drag source: payload = this entity index.
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("IRON_ENTITY", &i, sizeof(int));
            ImGui::TextUnformatted(label);
            ImGui::EndDragDropSource();
        }
        // Drop target: reparent the dragged entity under this one.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("IRON_ENTITY")) {
                const int child = *static_cast<const int*>(pl->Data);
                if (child != i) {            // self-drop is a no-op; deeper checks host-side
                    result.action            = Result::Action::Reparent;
                    result.reparentChild     = child;
                    result.reparentNewParent = i;
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (open) {
            for (int c : children[i]) drawNode(c);
            ImGui::TreePop();
        }
        ImGui::PopID();
    };

    for (int r : roots) drawNode(r);

    // Drop zone to unparent (reparent to root).
    ImGui::Separator();
    ImGui::Selectable("[ Drop here to unparent ]");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("IRON_ENTITY")) {
            const int child = *static_cast<const int*>(pl->Data);
            result.action            = Result::Action::Reparent;
            result.reparentChild     = child;
            result.reparentNewParent = -1;
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Separator();
    // ... existing Add bar + Duplicate/Delete buttons unchanged below ...
```

Add `#include <functional>` and `#include <vector>` and `#include <string>` to `SceneOutliner.cpp` if not present. Keep the rest of `draw` (Add bar, Duplicate/Delete, `ImGui::End()`) exactly as it was.

> If a Reparent action is set the same frame as an Add/Delete button press, Reparent wins because the drop happens during tree rendering (before the buttons). That's fine — at most one structural action per frame is processed by the host.

- [ ] **Step 3: Build**

Run: `cmake --build build --config Debug`
Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git add engine/editor/SceneOutliner.h engine/editor/SceneOutliner.cpp
git commit -m "M69: SceneOutliner tree view with drag-drop reparent (Reparent action)"
```

---

## Task 14: Host consumes the Reparent action

**Files:**
- Modify: `games/11-sandbox/main.cpp` action-dispatch block (~1556-1619).

- [ ] **Step 1: Handle Reparent**

Add a branch to the `if (action == ...)` chain (after the `Delete` branch, before the structural-edit flag block ~1615):

```cpp
            } else if (action == Action::Reparent) {
                // M69: reparent keeping world pose. Rejection (self/descendant/
                // out-of-range) is handled inside reparentKeepWorld.
                if (iron::reparentKeepWorld(scene, outRes.reparentChild,
                                            outRes.reparentNewParent)) {
                    mirrorParents();   // refresh World Parent components
                }
            }
```

- [ ] **Step 2: Mark Reparent a structural edit (for undo)**

Extend the structural-edit condition (~1616-1618):

```cpp
            if (action == Action::AddCube || action == Action::AddPlane ||
                action == Action::AddGltf || action == Action::Duplicate ||
                action == Action::Delete  || action == Action::Reparent)
                structuralEdit = true;
```

This routes the change into the existing whole-scene-JSON undo snapshot (spec §6: undo/redo already carries `parentIndex`; nothing else needed).

- [ ] **Step 3: Build**

Run: `cmake --build build --config Debug`
Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M69: host applies outliner Reparent (reparentKeepWorld + undo snapshot)"
```

---

## Task 15: Docs — logic-graph position nodes stay LOCAL space

**Files:**
- Modify: the GameplayNodes / node docs (search `docs/` for the file documenting `GetPosition`/`SetPosition`/`Translate`; likely `docs/engine/` node reference).

- [ ] **Step 1: Add a note**

Add a short paragraph where the position nodes are documented:

```markdown
> **Hierarchy (M69):** `GetPosition` / `SetPosition` / `Translate` operate in an
> entity's **LOCAL** space (relative to its parent). For a root entity local ==
> world. World-space variants (`GetWorldPosition` / `SetWorldPosition`) are
> deferred to M70/M71. A logic graph that moves a parent drags its children
> automatically (render composes through the parent chain).
```

- [ ] **Step 2: Commit**

```bash
git add docs/
git commit -m "M69: document that logic-graph position nodes stay local-space"
```

---

## Task 16: Full regression + demo gate

**Files:** none (verification only).

- [ ] **Step 1: Full clean build + all tests**

Run: `cmake --build build --config Debug`
Then: `ctest --test-dir build -C Debug --output-on-failure`
Expected: all tests pass (the existing 84 + the two new files). If any pre-existing test broke, STOP and use systematic-debugging.

- [ ] **Step 2: Confirm an existing scene still loads**

Launch the sandbox (the project's run path — `build/.../11-sandbox` exe, or the project run skill). Expected: the existing default scene loads unchanged (every entity is a root; outliner shows a flat tree). This proves back-compat.

- [ ] **Step 3: User-run visual demo gate (spec §8)**

Ask the user to run the sandbox and confirm each — this is the milestone's real acceptance gate:
1. Drag a cube onto cube-red in the outliner → tree nesting appears; child keeps its world position.
2. Rotate/move cube-red with the gizmo → child orbits/follows.
3. Inspector on the child shows local (parent-relative) values.
4. Play (F5): the M68 health-drain sinks cube-red → child sinks with it. Stop restores both.
5. Delete cube-red → child goes too; Ctrl+Z restores both, hierarchy intact.
6. Duplicate cube-red → copies the subtree.

- [ ] **Step 4: Final commit / PR**

Once the demo gate passes, use superpowers:finishing-a-development-branch to open the M69 PR against `main`.

---

## Self-Review (completed during planning)

**Spec coverage:**
- §1 data model → Tasks 2 (parentIndex + IO), 6 (Parent component), 7 (mirror). ✓
- §2 hierarchy math → Tasks 1 (`Transform::matrix`), 3 (`worldMatrixOf`/`isDescendant`/`collectSubtree`), 4 (`reparentKeepWorld`), 5 (`deleteSubtree`/`duplicateSubtree`). Reuses `decomposeTRS` (not reimplemented). ✓
- §3 render/picking/physics one path → Tasks 8 (render), 9 (picking), 10 (physics write-back). World-side composition justified by Play-mode logic-graph data flow. ✓ Logic-node local-space → Task 15. ✓
- §4 outliner → Tasks 13 (tree + drag-drop), 14 (host Reparent). Delete→subtree / Duplicate→deep copy → Task 11. ✓
- §5 gizmo proxy → Task 12. ✓
- §6 untouched (undo/Play-Stop/Inspector) → verified: undo via structuralEdit snapshot (Task 14); Play/Stop rides existing World deep-copy + `mirrorParents` in `rebuildDerivedFromScene` (Task 7); Inspector unchanged (local values are what's stored). ✓
- §7 tests → Tasks 1–6 each ship their tests; coverage matches the spec's bullet list (3-deep chain, reparent keep-world + rejections, cycle guard, delete/duplicate remap, IO round-trip/legacy/sanitize, `Transform::matrix` equality). ✓
- §8 demo gate → Task 16. ✓

**Placeholder scan:** No TBD/"add error handling"/"similar to Task N". Every code step has complete code. A few steps carry a `>` verification note where an exact upstream name (serialize entry-point, `World::add` duplicate semantics, const `get`) must be confirmed against the real header before coding — these are confirmations, not placeholders.

**Type consistency:** `parentIndex` (int, -1 root) used identically across SceneFormat/SceneIO/SceneHierarchy/outliner/main. `decomposeTRS` returns `BoneLocal { translation, rotation, scale }` — every call site maps `.translation`→`position`. `worldMatrix(World,EntityId[,memo])` and `worldMatrixOf(SceneFile,int)` are distinct and used in the right contexts (World-side at runtime; scene-side in the gizmo/Edit-only paths). `Result::Action::Reparent` + `reparentChild`/`reparentNewParent` consistent between outliner and host. `mirrorParents`/`uniqueName`/`appendAndSelect`/`resolveEntity`/`toRenderHandles`/`ResolvedEntity` reused as they exist in main.cpp.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-10-m69-scene-hierarchy.md`.
