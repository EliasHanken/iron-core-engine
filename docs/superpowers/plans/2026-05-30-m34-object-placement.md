# M34 — Object Placement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add / delete / duplicate entities in the editor — spawn a cube/plane/glTF in front of the camera, delete or duplicate the selection — then place with the gizmo and Save.

**Architecture:** `SceneOutliner` grows an add bar and returns the user's *intent* (`OutlinerResult`); the sandbox host executes it (mutating `scene.entities` + the `resolved` vector). The startup resolve-loop body is factored into a `resolveEntity` helper the host reuses for new entities; delete reindexes `resolved[].entityIndex`.

**Tech Stack:** C++23, Vulkan, Dear ImGui, the M29 `SceneFile`/glTF loader. No new deps.

**Spec:** `docs/superpowers/specs/2026-05-30-m34-object-placement-design.md`

**Branch:** `feat/m34-object-placement` (off `main` at the M33 merge `94857a6`, spec committed).

---

## Verified ground-truth (match exactly)

```cpp
// engine/scene/SceneFormat.h: SceneEntity{std::string name; Vec3 position; Quat rotation; Vec3 scale; MeshRef mesh; MaterialDef material;};
//   MeshRef{std::optional<PrimitiveKind> primitive; std::string gltfPath;}; PrimitiveKind{Cube,Plane}; MaterialDef defaults are fine.
// engine/scene/FreeFlyCamera.h: Vec3 position; Vec3 forward() const;
// ImGui 1.92: ImGui::InputText(label, buf, bufSize), ImGui::BeginDisabled(bool)/EndDisabled(), ImGui::SameLine().
// Sandbox already has (games/11-sandbox/main.cpp): exeDir, scene (mutable SceneFile), renderer, cam (FreeFlyCamera),
//   struct ResolvedEntity{int entityIndex; MeshHandle mesh; Material material; Mat4 model; Aabb localBounds;}; std::vector<ResolvedEntity> resolved;
//   lambdas primitiveMesh(PrimitiveKind)->MeshHandle and resolveTexture(path, fallback)->TextureHandle;
//   int selectedIndex; iron::SceneOutliner outliner; the render lambda calls outliner.draw(scene, selectedIndex).
// iron::loadGltfModel, iron::meshBounds, iron::loadRoughnessAsSpec, iron::translation/scaling, iron::Aabb, iron::Log all available + included.
```

---

## File Structure

**Modify:**
- `engine/editor/SceneOutliner.h` / `.cpp` — `Result` struct + add bar; `draw` returns `Result`.
- `games/11-sandbox/main.cpp` — `resolveEntity` helper (refactor startup loop onto it); `uniqueName`/`spawnPos` helpers; action handling (add/delete/duplicate + reindex); Delete/Ctrl+D; updated `outliner.draw` call site.
- `docs/engine/editor.md` — document add/delete/duplicate (Task 3).

No new headless tests — the add/delete/reindex logic is host-side and the add bar is editor-UI (neither reachable by the `ironcore`-linked harness). The existing 46 stay green.

---

## Task 1: SceneOutliner add bar + intent result

**Files:**
- Modify: `engine/editor/SceneOutliner.h`, `engine/editor/SceneOutliner.cpp`

- [ ] **Step 1: Replace `engine/editor/SceneOutliner.h`**

```cpp
#pragma once

#include <string>

namespace iron {

struct SceneFile;

// Lists the scene's entities + a Save button + an add bar (add cube/plane/glTF,
// duplicate, delete). Pure UI: it mutates the selection index and returns the
// user's intent for this frame; the host performs the actual mutation/resolve
// (it owns the renderer + the resolved render data).
class SceneOutliner {
public:
    struct Result {
        enum class Action { None, AddCube, AddPlane, AddGltf, Delete, Duplicate };
        bool        saveClicked = false;
        Action      action = Action::None;
        std::string gltfPath;   // populated for AddGltf (from the path text field)
    };

    // `selectedIndex` is updated in place when the user clicks an entity row.
    Result draw(const SceneFile& scene, int& selectedIndex);

private:
    char gltfPathBuf_[256] = {};   // ImGui text buffer for the glTF path field
};

}  // namespace iron
```

- [ ] **Step 2: Replace `engine/editor/SceneOutliner.cpp`**

```cpp
#include "editor/SceneOutliner.h"

#include "scene/SceneFormat.h"

#include <imgui.h>

namespace iron {

SceneOutliner::Result SceneOutliner::draw(const SceneFile& scene, int& selectedIndex) {
    Result result;
    ImGui::Begin("Scene Outliner");

    if (ImGui::Button("Save Scene")) result.saveClicked = true;
    ImGui::Separator();

    // Entity list.
    for (int i = 0; i < static_cast<int>(scene.entities.size()); ++i) {
        const std::string& name = scene.entities[i].name;
        const char* label = name.empty() ? "(unnamed)" : name.c_str();
        ImGui::PushID(i);
        if (ImGui::Selectable(label, i == selectedIndex)) selectedIndex = i;
        ImGui::PopID();
    }

    ImGui::Separator();

    // Add bar.
    if (ImGui::Button("+ Cube"))  result.action = Result::Action::AddCube;
    ImGui::SameLine();
    if (ImGui::Button("+ Plane")) result.action = Result::Action::AddPlane;

    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("##gltfpath", gltfPathBuf_, sizeof(gltfPathBuf_));
    ImGui::SameLine();
    if (ImGui::Button("+ glTF") && gltfPathBuf_[0] != '\0') {
        result.action   = Result::Action::AddGltf;
        result.gltfPath = gltfPathBuf_;
    }

    const bool hasSelection = selectedIndex >= 0 &&
                              selectedIndex < static_cast<int>(scene.entities.size());
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button("Duplicate")) result.action = Result::Action::Duplicate;
    ImGui::SameLine();
    if (ImGui::Button("Delete"))    result.action = Result::Action::Delete;
    ImGui::EndDisabled();

    ImGui::End();
    return result;
}

}  // namespace iron
```

- [ ] **Step 3: Build the editor lib**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target ironcore_editor --config Debug
```

Expected: clean compile. NOTE: this changes `draw`'s return type, so the `sandbox` target will NOT compile until Task 2 updates the call site — that's expected; build ONLY `ironcore_editor` here.

- [ ] **Step 4: Commit**

```powershell
git add engine/editor/SceneOutliner.h engine/editor/SceneOutliner.cpp
git commit -m "M34 Task 1: SceneOutliner add bar + OutlinerResult intent"
```

---

## Task 2: Sandbox host — resolveEntity helper + add/delete/duplicate

**Files:**
- Modify: `games/11-sandbox/main.cpp`

- [ ] **Step 1: Refactor the startup resolve loop into a `resolveEntity` helper**

Find the whole startup resolve loop — it begins at `for (int ei = 0; ei < static_cast<int>(scene.entities.size()); ++ei) {` and ends at the matching `}` just before the `iron::Log::info("sandbox: resolved %zu / %zu ...")` line. Replace that ENTIRE loop with a `resolveEntity` lambda followed by a short loop that calls it:

```cpp
    // Resolve one SceneEntity into a ResolvedEntity (mesh handle + material +
    // bounds + model). Returns false if the entity can't be drawn (e.g. a glTF
    // that fails to load, or no mesh). Reused for both the initial scene and
    // entities added at runtime.
    auto resolveEntity = [&](const iron::SceneEntity& e, int entityIndex,
                              ResolvedEntity& out) -> bool {
        out = ResolvedEntity{};
        out.entityIndex = entityIndex;

        if (e.mesh.primitive.has_value()) {
            out.mesh = primitiveMesh(e.mesh.primitive.value());
            if (e.mesh.primitive.value() == iron::PrimitiveKind::Cube)
                out.localBounds = iron::Aabb{iron::Vec3{-0.5f, -0.5f, -0.5f}, iron::Vec3{0.5f, 0.5f, 0.5f}};
            else  // Plane: unit quad in XZ; tiny Y thickness so it stays ray-pickable
                out.localBounds = iron::Aabb{iron::Vec3{-0.5f, -0.01f, -0.5f}, iron::Vec3{0.5f, 0.01f, 0.5f}};

        } else if (!e.mesh.gltfPath.empty()) {
            const std::string fullPath = exeDir + "/" + e.mesh.gltfPath;
            const auto gltfModel = iron::loadGltfModel(fullPath);
            if (!gltfModel) {
                iron::Log::warn("sandbox: entity '%s' gltf '%s' failed to load",
                                e.name.c_str(), fullPath.c_str());
                return false;
            }
            out.mesh = renderer.createMesh(gltfModel->mesh);
            out.localBounds = iron::meshBounds(gltfModel->mesh);
            out.material.texture   = gltfModel->materialPaths.albedo.empty()
                ? renderer.whiteTexture()
                : renderer.loadTexture(gltfModel->materialPaths.albedo);
            out.material.normalMap = gltfModel->materialPaths.normal.empty()
                ? renderer.flatNormalTexture()
                : renderer.loadTexture(gltfModel->materialPaths.normal);
            out.material.specularMap = renderer.noSpecularTexture();
            if (!gltfModel->materialPaths.metalRoughness.empty()) {
                int w = 0, h = 0;
                auto specBytes = iron::loadRoughnessAsSpec(
                    gltfModel->materialPaths.metalRoughness, w, h);
                if (!specBytes.empty())
                    out.material.specularMap = renderer.createTexture(w, h, specBytes.data());
            }

        } else {
            iron::Log::warn("sandbox: entity '%s' has no mesh", e.name.c_str());
            return false;
        }

        // Scene-file material overrides (textures fill still-invalid slots).
        if (out.material.texture == iron::kInvalidHandle)
            out.material.texture = resolveTexture(e.material.albedoPath, renderer.whiteTexture());
        if (out.material.normalMap == iron::kInvalidHandle)
            out.material.normalMap = resolveTexture(e.material.normalPath, renderer.flatNormalTexture());
        if (out.material.specularMap == iron::kInvalidHandle)
            out.material.specularMap = resolveTexture(e.material.specularPath, renderer.noSpecularTexture());
        out.material.emissive     = e.material.emissive;
        out.material.uvScale      = e.material.uvScale;
        out.material.reflectivity = e.material.reflectivity;
        out.model = iron::translation(e.position) * e.rotation.toMat4() * iron::scaling(e.scale);
        return true;
    };

    for (int ei = 0; ei < static_cast<int>(scene.entities.size()); ++ei) {
        ResolvedEntity re;
        if (resolveEntity(scene.entities[ei], ei, re)) resolved.push_back(re);
    }
```

(The `iron::Log::info("sandbox: resolved %zu / %zu entities from scene", ...)` line stays right after.)

- [ ] **Step 2: Add `uniqueName` / `spawnPos` / `appendAndSelect` helpers**

Find the `gizmoOriginFor` lambda (just before `// --- Main loop ---`). Immediately AFTER it, add:

```cpp
    // Generate a scene-unique entity name from a base ("cube" -> "cube", "cube 2"...).
    auto uniqueName = [&](const std::string& base) -> std::string {
        auto taken = [&](const std::string& n) {
            for (const auto& e : scene.entities) if (e.name == n) return true;
            return false;
        };
        if (!taken(base)) return base;
        for (int i = 2; ; ++i) {
            const std::string n = base + " " + std::to_string(i);
            if (!taken(n)) return n;
        }
    };

    // Where a freshly added entity appears: a few units in front of the camera.
    auto spawnPos = [&]() -> iron::Vec3 {
        return cam.position + cam.forward() * 5.0f;
    };

    // Append a fully-built entity, resolve it, and select it. Rolls back if the
    // resolve fails (e.g. a bad glTF path).
    auto appendAndSelect = [&](iron::SceneEntity ne) {
        const int idx = static_cast<int>(scene.entities.size());
        scene.entities.push_back(ne);
        ResolvedEntity re;
        if (resolveEntity(scene.entities[idx], idx, re)) {
            resolved.push_back(re);
            selectedIndex = idx;
        } else {
            scene.entities.pop_back();
            iron::Log::warn("sandbox: add failed; entity not added");
        }
    };
```

- [ ] **Step 3: Update the `outliner.draw` call site + handle actions**

In the render lambda, find:

```cpp
        const bool saveClicked = outliner.draw(scene, selectedIndex);
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            inspector.draw(scene.entities[selectedIndex]);
        environment.draw(scene);
        if (saveClicked) {
            if (iron::saveSceneFile(scene, scenePath))
                iron::Log::info("sandbox: saved %s", scenePath.c_str());
            else
                iron::Log::error("sandbox: save FAILED for %s", scenePath.c_str());
        }
```

Replace it with:

```cpp
        const iron::SceneOutliner::Result outRes = outliner.draw(scene, selectedIndex);
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(scene.entities.size()))
            inspector.draw(scene.entities[selectedIndex]);
        environment.draw(scene);
        if (outRes.saveClicked) {
            if (iron::saveSceneFile(scene, scenePath))
                iron::Log::info("sandbox: saved %s", scenePath.c_str());
            else
                iron::Log::error("sandbox: save FAILED for %s", scenePath.c_str());
        }

        // --- add / delete / duplicate (Outliner buttons OR keyboard shortcuts) ---
        using Action = iron::SceneOutliner::Result::Action;
        Action action = outRes.action;
        if (action == Action::None && !imgui.wantsKeyboard()) {
            iron::Input& kin = app.input();
            if (kin.keyPressed(GLFW_KEY_DELETE)) action = Action::Delete;
            else if (kin.keyDown(GLFW_KEY_LEFT_CONTROL) && kin.keyPressed(GLFW_KEY_D))
                action = Action::Duplicate;
        }
        const bool selValid = selectedIndex >= 0 &&
                              selectedIndex < static_cast<int>(scene.entities.size());
        if (action == Action::AddCube) {
            iron::SceneEntity ne;
            ne.name = uniqueName("cube");
            ne.position = spawnPos();
            ne.mesh.primitive = iron::PrimitiveKind::Cube;
            appendAndSelect(ne);
        } else if (action == Action::AddPlane) {
            iron::SceneEntity ne;
            ne.name = uniqueName("plane");
            ne.position = spawnPos();
            ne.mesh.primitive = iron::PrimitiveKind::Plane;
            appendAndSelect(ne);
        } else if (action == Action::AddGltf) {
            std::string stem = outRes.gltfPath;
            const auto slash = stem.find_last_of("/\\");
            if (slash != std::string::npos) stem = stem.substr(slash + 1);
            const auto dot = stem.find_last_of('.');
            if (dot != std::string::npos) stem = stem.substr(0, dot);
            iron::SceneEntity ne;
            ne.name = uniqueName(stem.empty() ? "gltf" : stem);
            ne.position = spawnPos();
            ne.mesh.gltfPath = outRes.gltfPath;
            appendAndSelect(ne);
        } else if (action == Action::Duplicate && selValid) {
            iron::SceneEntity ne = scene.entities[selectedIndex];  // copy mesh+material+transform
            ne.name = uniqueName(ne.name);
            ne.position.x += 0.5f;  // slight offset so the copy is visible
            appendAndSelect(ne);
        } else if (action == Action::Delete && selValid) {
            const int d = selectedIndex;
            scene.entities.erase(scene.entities.begin() + d);
            // Drop the deleted entity's resolved entry; shift higher indices down.
            for (std::size_t i = 0; i < resolved.size();) {
                if (resolved[i].entityIndex == d) { resolved.erase(resolved.begin() + i); continue; }
                if (resolved[i].entityIndex > d) --resolved[i].entityIndex;
                ++i;
            }
            selectedIndex = -1;
        }
```

(The re-derive loop, `beginFrame`, submit, gizmo draw, selection outline, etc. that follow stay unchanged — they run after the mutation each frame, so `resolved` and `scene.entities` are consistent.)

- [ ] **Step 4: Build**

```powershell
cmake -S . -B build-vk
cmake --build build-vk --target sandbox --config Debug
```

Expected: clean build of `sandbox` (the `outliner.draw` call now matches the new `Result` return type; `std::string`/`std::to_string` are already available via `<string>`).

- [ ] **Step 5: Full suite — no regressions**

```powershell
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 46/46 green (no engine test touched).

- [ ] **Step 6: Visual check (human verifies; subagent confirms build only)**

```powershell
.\build-vk\games\11-sandbox\Debug\sandbox.exe
```

Expected (user-verified): `+ Cube`/`+ Plane` spawn a primitive ~5 units in front of the camera, auto-named + selected with its gizmo; typing a valid path + `+ glTF` adds a model (bad path → warning, nothing added); `Duplicate` makes an offset copy; `Delete` (button or Delete key) removes the selection and other entities stay correctly selectable; Save → relaunch shows the new scene. The subagent only confirms the build succeeds.

- [ ] **Step 7: Commit**

```powershell
git add games/11-sandbox/main.cpp
git commit -m "M34 Task 2: sandbox add/delete/duplicate entities (resolveEntity + reindex)"
```

---

## Task 3: Docs + PR

**Files:**
- Modify: `docs/engine/editor.md`

- [ ] **Step 1: Document add/delete/duplicate in `docs/engine/editor.md`**

Read the existing doc and add a short "Placing objects" subsection covering: the Outliner add bar (`+ Cube` / `+ Plane` / glTF path + `+ glTF`, `Duplicate`, `Delete`); new entities spawn in front of the camera, auto-named + selected; Delete key + Ctrl+D shortcuts; the Outliner-emits-intent / host-executes split (`SceneOutliner::Result`); and the v1 limitations (no undo, deleting a glTF leaks its GPU mesh, glTF added by path relative to the exe dir). Match the doc's prose style.

- [ ] **Step 2: Commit, push, open PR**

```powershell
git add docs/engine/editor.md
git commit -m "M34: document object placement (add/delete/duplicate)"
git push -u origin feat/m34-object-placement
```

Open the PR (match the M33 #53 template). Title: `M34: Object placement (add / delete / duplicate)`. Body:

```
## Summary

- `SceneOutliner` gains an add bar (`+ Cube` / `+ Plane` / glTF path + `+ glTF`, `Duplicate`, `Delete`) and returns the user's intent (`OutlinerResult`); the sandbox host executes it.
- Add / duplicate build a `SceneEntity`, append it, resolve it (new `resolveEntity` helper, shared with the startup load), and select it; delete erases the entity + its resolved entry and reindexes `resolved[].entityIndex`.
- New entities spawn ~5 units in front of the camera, auto-named, and selected with the gizmo. Delete key + Ctrl+D shortcuts. Save persists via `saveSceneFile`.

## Test plan

- [x] Full suite green (46/46) — placement logic is host-side / editor-UI, no engine test touched
- [x] ironcore_editor + sandbox build clean
- [ ] Visual: add cube/plane/glTF (bad path warns); duplicate offsets a copy; delete removes + keeps selection correct; Save persists; relaunch loads the new scene

## Known v1 limitations

- No undo/redo, multi-select, parenting, or asset browser (glTF added by path field).
- Deleting a glTF entity leaks its GPU mesh (no destroyMesh API yet).
- glTF paths resolve relative to the executable directory (same as M29).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

---

## Verification Checklist (before declaring done)

- [ ] `ctest --test-dir build-vk -C Debug --output-on-failure` — 46/46 green.
- [ ] `ironcore_editor` + `sandbox` build clean.
- [ ] Visual: add cube/plane/glTF in front of camera + selected; duplicate; delete (button + key) keeps the resolved↔entities mapping correct (other entities still pick right); Save → relaunch shows the augmented scene.
- [ ] PR CI green.
