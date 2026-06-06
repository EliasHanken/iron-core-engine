# M59 — Node Editor UX v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the node editor the exact UE4-blueprint feel — a gradient header, drag-from-a-pin → type-compatible auto-wired create menu, and right-click context menus — replacing the "Add" toolbar palette.

**Architecture:** No new vendoring. A headless `compatibleCreations` query (reusing a factored `portsCompatible` rule shared with `connect()`) drives the create menu; `NodeGraphPanel.cpp` adds the gradient header (`AddImageRounded` with a generated gradient texture, or a multicolor fallback), the `QueryNewNode` drag-create popup, and the `Show*ContextMenu` right-click popups (via `Suspend`/`Resume`). All graph mutations stay routed through `GraphEditorModel` so M57 undo + M58 comment serialization keep working.

**Tech Stack:** C++17, ImGui 1.92.8, thedmd/imgui-node-editor (vendored), CTest. Build dir `build-vk`, `--config Debug`, `ctest -C Debug`. Bash tool is bash; prefix commands with `cd "C:/Users/elias/Documents/_dev/iron-core-engine" &&`. Branch `m59-node-editor-ux` (create off `main` AFTER M58 PR #90 merges — M59 builds on M58's NodeGraphPanel header/comment code). Commit trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` (tmp file + `git commit -F`; never `git add` under `tmp/`).

---

## File Structure

- **`engine/nodes/NodeRegistry.h` / `.cpp`** (MODIFY) — declare/define `bool portsCompatible(const PortDesc& from, const PortDesc& to)` (the single source of truth for "can these two ports connect"), reused by `connect()` and `compatibleCreations`.
- **`engine/nodes/GraphEditorModel.h` / `.cpp`** (MODIFY) — `NodeCreation` struct + `compatibleCreations(PortType, PortDir)`; refactor `connect()` to call `portsCompatible`.
- **`engine/editor/NodeGraphPanel.h` / `.cpp`** (MODIFY) — gradient header; drag-create popup; right-click menus; remove the Add toolbar palette.
- **`engine/editor/ImGuiLayer.h` / `.cpp`** (MODIFY, only if the texture path is taken) — `registerTexture` helper for the gradient.
- **`tests/test_graph_editor.cpp`** (MODIFY) — `portsCompatible` parity + `compatibleCreations` connectability tests.

---

### Task 1: `portsCompatible` + `compatibleCreations` (headless, TDD)

**Files:**
- Modify: `engine/nodes/NodeRegistry.h`, `engine/nodes/NodeRegistry.cpp`
- Modify: `engine/nodes/GraphEditorModel.h`, `engine/nodes/GraphEditorModel.cpp`
- Test: `tests/test_graph_editor.cpp`

- [ ] **Step 1: Declare `portsCompatible` (NodeRegistry.h) and `NodeCreation` + `compatibleCreations` (GraphEditorModel.h)**

In `engine/nodes/NodeRegistry.h`, after the `PortDesc` struct, add:
```cpp
// True iff a connection from `from` (must be an Out pin) to `to` (must be an In
// pin) is valid: exec<->exec or data<->data with int/float interchangeable. The
// single source of truth shared by GraphEditorModel::connect and the drag-create
// menu so the two never diverge.
bool portsCompatible(const PortDesc& from, const PortDesc& to);
```
In `engine/nodes/GraphEditorModel.h`, add the struct before `class GraphEditorModel` (inside `namespace iron`):
```cpp
// A node type creatable from a dragged pin, paired with the port on that type to
// auto-wire to. Produced by GraphEditorModel::compatibleCreations.
struct NodeCreation {
    std::string typeName;
    std::string targetPort;
};
```
Add the public method (after `compatibleCreations` belongs near the query methods, e.g. after `comments()`):
```cpp
    // Node types creatable from a pin of (srcType, srcDir), each paired with the
    // first port on that type forming a valid connection (same rule as connect()).
    // Drives the drag-from-pin create menu.
    std::vector<NodeCreation> compatibleCreations(PortType srcType, PortDir srcDir) const;
```

- [ ] **Step 2: Write the failing tests**

In `tests/test_graph_editor.cpp`, add includes if missing (`#include "nodes/NodeRegistry.h"` is already transitively available via GraphEditorModel.h). Add inside `main()` before the final `return iron_test_result();`:
```cpp
    // M59: portsCompatible parity (pure, no registry).
    {
        const PortDesc outF{"o", PortType::Float, PortDir::Out};
        const PortDesc inF {"i", PortType::Float, PortDir::In};
        const PortDesc inI {"i", PortType::Int,   PortDir::In};
        const PortDesc outE{"o", PortType::Exec,  PortDir::Out};
        const PortDesc inE {"i", PortType::Exec,  PortDir::In};
        CHECK(portsCompatible(outF, inF));    // float->float
        CHECK(portsCompatible(outF, inI));    // float->int (numeric interchange)
        CHECK(portsCompatible(outE, inE));    // exec->exec
        CHECK(!portsCompatible(outF, inE));   // data->exec mismatch
        CHECK(!portsCompatible(outE, inF));   // exec->data mismatch
        CHECK(!portsCompatible(outF, outF));  // Out->Out (target not an input)
        CHECK(!portsCompatible(inF, inF));    // In as source (from must be Out)
    }

    // M59: compatibleCreations — every offered creation is genuinely connectable.
    {
        GraphEditorModel m(&reg);
        // Const has a Float output "out" (used elsewhere in this test file).
        const NodeId src = m.addNode("Const", 0, 0);
        const auto fromFloat = m.compatibleCreations(PortType::Float, PortDir::Out);
        CHECK(!fromFloat.empty());
        for (const auto& c : fromFloat) {
            const NodeId n = m.addNode(c.typeName, 0, 0);
            CHECK(m.connect(src, "out", n, c.targetPort));   // must actually connect
            m.deleteNode(n);
        }
        // Exec source offers a flow node (Branch has an exec "in") but not a
        // data-only node (Const has no exec input).
        const auto fromExec = m.compatibleCreations(PortType::Exec, PortDir::Out);
        bool hasBranch = false, hasConst = false;
        for (const auto& c : fromExec) {
            if (c.typeName == "Branch") hasBranch = true;
            if (c.typeName == "Const")  hasConst  = true;
        }
        CHECK(hasBranch);
        CHECK(!hasConst);
    }
```
(`reg` is the file's existing `NodeRegistry`. `PortDesc`/`PortDir`/`PortType` are in scope via the engine headers.)

- [ ] **Step 3: Run to verify it fails**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target test_graph_editor --config Debug
```
Expected: FAIL — `portsCompatible` / `compatibleCreations` undeclared/unresolved.

- [ ] **Step 4: Implement `portsCompatible` + refactor `connect` + implement `compatibleCreations`**

In `engine/nodes/NodeRegistry.cpp`, add (include `nodes/NodeRegistry.h` is already there):
```cpp
namespace {
bool dataCompatible(PortType a, PortType b) {
    if (a == b) return true;
    const bool aNum = (a == PortType::Int || a == PortType::Float);
    const bool bNum = (b == PortType::Int || b == PortType::Float);
    return aNum && bNum;
}
}  // namespace

bool portsCompatible(const PortDesc& from, const PortDesc& to) {
    if (from.dir != PortDir::Out || to.dir != PortDir::In) return false;
    const bool fromExec = from.type == PortType::Exec;
    const bool toExec   = to.type   == PortType::Exec;
    if (fromExec != toExec) return false;
    if (!fromExec && !dataCompatible(from.type, to.type)) return false;
    return true;
}
```
In `engine/nodes/GraphEditorModel.cpp`, replace the file-local `dataCompatible` + the inline checks in `connect()` so `connect` uses the shared rule. Change the body of `connect` from the `if (from->dir != ...)` / exec / dataCompatible block to:
```cpp
    const PortDesc* from = portOf(fromNode, fromPort);
    const PortDesc* to   = portOf(toNode, toPort);
    if (!from || !to) return false;
    if (!portsCompatible(*from, *to)) return false;

    if (from->type == PortType::Exec) {
        graph_.removeOutgoing(fromNode, fromPort);        // exec out: one target
    } else {
        graph_.disconnect(toNode, toPort);                // data in: one source
    }
    graph_.connect(fromNode, std::move(fromPort), toNode, std::move(toPort));
    dirty_ = true;
    return true;
```
Delete the now-unused file-local `dataCompatible` from GraphEditorModel.cpp (lines ~39-46). Add `compatibleCreations`:
```cpp
std::vector<NodeCreation> GraphEditorModel::compatibleCreations(PortType srcType,
                                                                PortDir srcDir) const {
    std::vector<NodeCreation> out;
    if (!registry_) return out;
    const PortDesc src{"", srcType, srcDir};
    for (const NodeTypeDesc* t : registry_->all()) {
        for (const PortDesc& p : t->ports) {
            // Source Out -> the type's In port; source In -> the type's Out port.
            const bool ok = (srcDir == PortDir::Out) ? portsCompatible(src, p)
                                                      : portsCompatible(p, src);
            if (ok) { out.push_back({t->typeName, p.name}); break; }  // first match per type
        }
    }
    return out;
}
```
(`compatibleCreations` is `const`; `portOf` used by `connect` is unchanged. Ensure `nodes/NodeRegistry.h` is included in GraphEditorModel.cpp — it is, via GraphEditorModel.h.)

- [ ] **Step 5: Run to verify it passes**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target test_graph_editor --config Debug && ctest --test-dir build-vk -C Debug -R test_graph_editor --output-on-failure
```
Expected: PASS, including the new blocks. (If `compatibleCreations(Exec,Out)` unexpectedly includes Const, inspect Const's ports — the test asserts data-only types are excluded from an exec source.)

- [ ] **Step 6: Commit**

Stage `engine/nodes/NodeRegistry.h`, `engine/nodes/NodeRegistry.cpp`, `engine/nodes/GraphEditorModel.h`, `engine/nodes/GraphEditorModel.cpp`, `tests/test_graph_editor.cpp`. Subject: `M59: portsCompatible + compatibleCreations (shared connect rule)`.

---

### Task 2: Gradient header

**Files:**
- Modify: `engine/editor/NodeGraphPanel.h`, `engine/editor/NodeGraphPanel.cpp`
- Modify (only if texture path): `engine/editor/ImGuiLayer.h`, `engine/editor/ImGuiLayer.cpp`

No unit test (visual-gated).

- [ ] **Step 1: Decide the binding path (spike, ~10 min)**

Read `engine/editor/ImGuiLayer.{h,cpp}` (`viewportTexture(VkImageView, VkSampler)` and how it calls `ImGui_ImplVulkan_AddTexture`) and `engine/render/backends/vulkan/VulkanRenderer`/`VkTexture` for whether a `TextureHandle`'s `VkImageView`+`VkSampler` are reachable. Decide:
- **Texture path** if you can create a small RGBA texture and obtain a `VkImageView`+`VkSampler` to feed an `ImGui_ImplVulkan_AddTexture`-style binding. Then add `ImGuiLayer::registerTexture` (Step 2a).
- **Multicolor fallback** otherwise (Step 2b). Both produce a category-tinted gradient header; the fallback needs zero Vulkan plumbing.

Record the choice in the commit message.

- [ ] **Step 2a: (texture path) add `ImGuiLayer::registerTexture` + draw with `AddImageRounded`**

In `engine/editor/ImGuiLayer.h`, after `viewportTexture`:
```cpp
    // Upload a small RGBA8 texture and return an ImGui texture id (void* /
    // VkDescriptorSet), valid for the layer's lifetime. Used for the node header
    // gradient. Returns nullptr if not initialized.
    void* registerTexture(const unsigned char* rgba, int width, int height);
```
Implement in `ImGuiLayer.cpp` by creating a `VkImage`+view+sampler from the bytes (staging upload + layout transition) and `ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)`, mirroring the existing texture handling; store created handles for cleanup in shutdown. (If this balloons past ~60 lines or needs renderer internals you can't reach, abandon the texture path and use Step 2b.)

In `NodeGraphPanel.h`, add a member `void* headerTex_ = nullptr;`. In `NodeGraphPanel.cpp`, generate the gradient once (lazily on first `draw`, needs the ImGuiLayer — pass it in, or generate the RGBA and bind via a host-provided id). Simplest: generate a 1×64 RGBA ramp (top `a=70` white fading to `a=0`) and bind in the panel ctor if it has the layer; otherwise the host registers it and passes the id to `draw`. Then in the header draw (replacing M58's `AddRectFilledMultiColor` sheen), keep the base `AddRectFilled(a,b, headerColor(cat), rounding, RoundCornersTop)` then overlay:
```cpp
            if (headerTex_)
                bg->AddImageRounded(reinterpret_cast<ImTextureID>(headerTex_),
                                    a, b, ImVec2(0,0), ImVec2(1,1),
                                    IM_COL32(255,255,255,90), rounding,
                                    ImDrawFlags_RoundCornersTop);
```
(The white ramp × the base category fill reads as the UE4 gradient.)

- [ ] **Step 2b: (fallback) refined multicolor gradient**

If not using a texture, replace M58's header sheen with a two-stop vertical gradient in the category hue. In the header draw, after computing `a`, `b`, `rounding`, `hdr = headerColor(t->category)`:
```cpp
            // Rounded-top base (keeps corners), then a vertical gradient inset by
            // the rounding so its square corners don't poke past the rounded base.
            bg->AddRectFilled(a, b, hdr, rounding, ImDrawFlags_RoundCornersTop);
            const ImU32 top = IM_COL32(255, 255, 255, 60);   // gloss highlight
            const ImU32 bot = IM_COL32(0, 0, 0, 40);         // subtle darken at base
            const ImVec2 gi(a.x + rounding, a.y);
            const ImVec2 gj(b.x - rounding, b.y);
            bg->AddRectFilledMultiColor(gi, gj, top, top, bot, bot);
            bg->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), IM_COL32(0,0,0,110), 1.0f);
```

- [ ] **Step 3: Build + commit**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target sandbox --config Debug
```
Stage the touched files. Subject: `M59: gradient node header (<texture|multicolor> path)`.

---

### Task 3: Drag-from-pin → type-compatible create menu

**Files:**
- Modify: `engine/editor/NodeGraphPanel.h`, `engine/editor/NodeGraphPanel.cpp`

No unit test (interaction; the compat core is Task 1's tests; visual-gated).

- [ ] **Step 1: Add pending-create state to the header**

In `NodeGraphPanel.h`, add private members:
```cpp
    unsigned long long pendingCreatePin_ = 0;   // source pin id for the drag-create popup (0 = none)
    float pendingCreateX_ = 0.0f, pendingCreateY_ = 0.0f;  // canvas drop position
```
(`unsigned long long` avoids leaking node-editor types into the header; cast to `ed::PinId` in the .cpp.)

- [ ] **Step 2: Detect drag-to-empty in `ed::BeginCreate` and open the popup**

In `NodeGraphPanel.cpp`, inside the existing `if (ed::BeginCreate()) { ... }` block, AFTER the `QueryNewLink` handling, add `QueryNewNode`:
```cpp
        ed::PinId newNodePin = 0;
        if (ed::QueryNewNode(&newNodePin)) {
            if (ed::AcceptNewItem()) {
                const ImVec2 canvasPos = ed::ScreenToCanvas(ImGui::GetMousePos());
                pendingCreatePin_ = static_cast<unsigned long long>(newNodePin.Get());
                pendingCreateX_   = canvasPos.x;
                pendingCreateY_   = canvasPos.y;
                ed::Suspend();
                ImGui::OpenPopup("##create_node_popup");
                ed::Resume();
            }
        }
```
(`ed::ScreenToCanvas` converts the drop point to graph coordinates so the node spawns under the cursor. If that exact name isn't present, use `ed::GetHintForegroundDrawList`-adjacent coordinate helpers — check `imgui_node_editor.h` for `ScreenToCanvas`/`CanvasToScreen`; the header exposes coordinate conversion. If neither exists, fall back to the panel's `spawnX_/spawnY_`.)

- [ ] **Step 3: Render the create popup after `ed::End()`**

In `NodeGraphPanel.cpp`, AFTER `ed::End();` and BEFORE `ed::SetCurrentEditor(nullptr);`, add the popup (popups must render outside the canvas):
```cpp
    ed::Suspend();
    if (ImGui::BeginPopup("##create_node_popup")) {
        ImGui::TextDisabled("Create node");
        ImGui::Separator();
        NodeId pn; std::string pp;
        const PortDesc* sp = resolvePin(model, pendingCreatePin_, pn, pp);
        if (sp) {
            const auto options = model.compatibleCreations(sp->type, sp->dir);
            if (options.empty()) ImGui::TextDisabled("(no compatible nodes)");
            for (const auto& c : options) {
                if (ImGui::MenuItem(c.typeName.c_str())) {
                    const NodeId nn = model.addNode(c.typeName, pendingCreateX_, pendingCreateY_);
                    // Wire dragged pin -> new node, respecting direction.
                    if (sp->dir == PortDir::Out) model.connect(pn, pp, nn, c.targetPort);
                    else                          model.connect(nn, c.targetPort, pn, pp);
                }
            }
        }
        ImGui::EndPopup();
    }
    ed::Resume();
```
(`resolvePin` is the existing file-local helper taking a `std::uintptr_t` — `pendingCreatePin_` casts cleanly. `addNode(type, x, y)` already stores editor position; the new node appears at the drop point.)

- [ ] **Step 4: Build + commit**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target sandbox --config Debug
```
Verify against `third_party/imgui-node-editor/imgui_node_editor.h` that `ScreenToCanvas` exists (grep it); adjust per Step 2's note if not. Stage NodeGraphPanel.{h,cpp}. Subject: `M59: drag-from-pin create menu (type-compatible, auto-wire)`.

---

### Task 4: Right-click context menus + remove the Add toolbar palette

**Files:**
- Modify: `engine/editor/NodeGraphPanel.cpp`

No unit test (interaction; visual-gated).

- [ ] **Step 1: Trigger the context menus inside the canvas**

In `NodeGraphPanel.cpp`, AFTER the `ed::BeginDelete()/EndDelete()` block and BEFORE `ed::End();`, detect right-clicks and open popups (suspended):
```cpp
    {
        ed::NodeId ctxNode = 0; ed::PinId ctxPin = 0; ed::LinkId ctxLink = 0;
        ed::Suspend();
        if (ed::ShowNodeContextMenu(&ctxNode)) {
            ctxMenuNode_ = static_cast<unsigned long long>(ctxNode.Get());
            ImGui::OpenPopup("##node_ctx");
        } else if (ed::ShowPinContextMenu(&ctxPin)) {
            ctxMenuPin_ = static_cast<unsigned long long>(ctxPin.Get());
            ImGui::OpenPopup("##pin_ctx");
        } else if (ed::ShowLinkContextMenu(&ctxLink)) {
            ctxMenuLink_ = static_cast<unsigned long long>(ctxLink.Get());
            ImGui::OpenPopup("##link_ctx");
        } else if (ed::ShowBackgroundContextMenu()) {
            const ImVec2 cp = ed::ScreenToCanvas(ImGui::GetMousePos());
            ctxMenuBgX_ = cp.x; ctxMenuBgY_ = cp.y;
            ImGui::OpenPopup("##bg_ctx");
        }
        ed::Resume();
    }
```
Add the supporting members to `NodeGraphPanel.h`:
```cpp
    unsigned long long ctxMenuNode_ = 0, ctxMenuPin_ = 0, ctxMenuLink_ = 0;
    float ctxMenuBgX_ = 0.0f, ctxMenuBgY_ = 0.0f;
```

- [ ] **Step 2: Render the context popups after `ed::End()`**

In `NodeGraphPanel.cpp`, in the suspended region after `ed::End()` (alongside the Task 3 create popup, inside the same `ed::Suspend()`/`ed::Resume()` pair — or a second pair), add:
```cpp
        if (ImGui::BeginPopup("##bg_ctx")) {
            if (model.registry() && ImGui::BeginMenu("Add Node")) {
                // Group by category.
                for (const NodeTypeDesc* t : model.registry()->all())
                    if (ImGui::MenuItem(t->typeName.c_str()))
                        model.addNode(t->typeName, ctxMenuBgX_, ctxMenuBgY_);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Add Comment"))
                model.addComment(ctxMenuBgX_, ctxMenuBgY_, 240.0f, 160.0f, "Comment");
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##node_ctx")) {
            const std::uintptr_t raw = static_cast<std::uintptr_t>(ctxMenuNode_);
            const bool isComment = (raw & (std::uintptr_t{1} << 62)) != 0;
            if (isComment) {
                if (ImGui::MenuItem("Delete Comment")) {
                    const std::uint32_t cid = static_cast<std::uint32_t>(raw & 0xFFFFFFFFu);
                    model.deleteComment(cid);
                    placedComments_.erase(cid); commentOffX_.erase(cid); commentOffY_.erase(cid);
                }
            } else {
                const NodeId nid = static_cast<NodeId>(raw);
                if (ImGui::MenuItem("Delete")) model.deleteNode(nid);
                if (ImGui::MenuItem("Duplicate")) {
                    if (const Node* src = model.graph().node(nid)) {
                        const NodeId dup = model.addNode(src->typeName,
                                                         src->editorX + 30.0f, src->editorY + 30.0f);
                        for (const auto& kv : src->literals) model.setLiteral(dup, kv.first, kv.second);
                    }
                }
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##pin_ctx")) {
            NodeId pn; std::string pp;
            const PortDesc* sp = resolvePin(model, static_cast<std::uintptr_t>(ctxMenuPin_), pn, pp);
            if (sp && ImGui::MenuItem("Break links")) {
                if (sp->dir == PortDir::In) model.disconnect(pn, pp);
                else                        model.graph(); // out: handled below
            }
            // For an Out pin, break its outgoing link via the model.
            if (sp && sp->dir == PortDir::Out && ImGui::IsItemClicked()) {}
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##link_ctx")) {
            if (ImGui::MenuItem("Delete link")) {
                const std::size_t idx = static_cast<std::size_t>(ctxMenuLink_) - 1;
                const auto& cs = model.graph().connections();
                if (idx < cs.size()) model.disconnect(cs[idx].toNode, cs[idx].toPort);
            }
            ImGui::EndPopup();
        }
```
NOTE on the pin "Break links": the model exposes `disconnect(toNode, toPort)` (breaks an In pin's source) and `Graph::removeOutgoing(fromNode, fromPort)` (breaks an Out pin's target) — but `GraphEditorModel` has no public `removeOutgoing` wrapper. Add one to `GraphEditorModel` (`void disconnectOutgoing(NodeId fromNode, std::string fromPort)` calling `graph_.removeOutgoing(...)` + `dirty_=true`), and in the pin popup call `disconnect` for In pins and `disconnectOutgoing` for Out pins. Replace the placeholder Out branch above with that call. (This is a small, real addition — declare it in GraphEditorModel.h next to `disconnect` and define it in the .cpp.)

- [ ] **Step 3: Remove the Add toolbar palette**

In `NodeGraphPanel.cpp`, delete the palette block (the `if (model.registry()) { ImGui::TextUnformatted("Add:"); for (... SmallButton ...) ... "+ Comment" ... }` — lines ~143-160). Node + comment creation now lives in the right-click background menu and drag-create. Keep Run / Save / Load / Entity controls and the Run-outputs readout.

- [ ] **Step 4: Build + commit**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target sandbox --config Debug
```
Stage NodeGraphPanel.{h,cpp} + GraphEditorModel.{h,cpp} (for `disconnectOutgoing`). Subject: `M59: right-click context menus + remove Add toolbar palette`.

---

### Task 5: Full build, tests, visual gate, PR

**Files:** none.

- [ ] **Step 1: Clean full build** — `cmake --build build-vk --config Debug` (all targets).
- [ ] **Step 2: Full test sweep** — `ctest --test-dir build-vk -C Debug --output-on-failure`. Expected: all pass incl. the new `test_graph_editor` blocks. Record N/N.
- [ ] **Step 3: Code review** — `git diff main...HEAD`; focus on the `portsCompatible` refactor parity (connect behavior unchanged), the Suspend/Resume popup balancing, id namespacing in the context menus, and that all mutations route through `GraphEditorModel`.
- [ ] **Step 4: Visual gate** — launch the sandbox: gradient header matches the demo; drag from a pin onto empty canvas → menu of compatible nodes → pick → it spawns wired; right-click empty → Add Node/Add Comment; right-click node → Delete/Duplicate; right-click pin → Break links; right-click link → Delete; the Add toolbar palette is gone; undo/redo (Ctrl+Z) still covers all of it; Play still runs.
- [ ] **Step 5: PR** — push, open PR (base main, `🤖 Generated with [Claude Code]` footer), background CI-watch-merge (squash). Re-run CI on transient flakes (e.g. vcpkg 504). Update memory (`iron-core-engine-progress.md` + `MEMORY.md`).

---

## Self-Review

**Spec coverage:**
- Gradient header → Task 2 (texture or multicolor).
- Drag-from-pin type-compatible auto-wire → Task 1 (`compatibleCreations`) + Task 3 (popup + connect).
- Right-click menus (bg/node/pin/link) → Task 4.
- Remove Add palette → Task 4 Step 3.
- `portsCompatible` shared with `connect` → Task 1.
- Headless tests → Task 1; rest visual-gated → Task 5.
- Mutations through `GraphEditorModel` (undo/comments intact) → all tasks use `addNode`/`connect`/`disconnect`/`deleteNode`/`addComment`/`disconnectOutgoing`.

**Placeholder scan:** The pin "Break links" Out branch was a placeholder in the first draft — Task 4 Step 2's NOTE resolves it with a concrete `disconnectOutgoing` addition. No other TBDs. The `ScreenToCanvas` name is flagged to verify against the vendored header (Task 3 Step 2 / Task 4 Step 1) with a fallback.

**Type consistency:** `portsCompatible(const PortDesc&, const PortDesc&)` and `NodeCreation{typeName,targetPort}` and `compatibleCreations(PortType,PortDir)` are consistent across Tasks 1/3/4. `disconnectOutgoing(NodeId, std::string)` is declared+used consistently in Task 4. Pending-create + context-menu members (`pendingCreatePin_`, `ctxMenu*_`) are declared in the header (Task 3 Step 1 / Task 4 Step 1) and used in the .cpp. Comment bit-62 namespace matches M58.

**Open verification (do at implementation, not a blocker):** confirm `ed::ScreenToCanvas` exists in the vendored header (grep), and the exact builtin node port names used by the Task 1 test (the connectability loop is self-verifying, so it won't assert on a wrong name — it just exercises whatever `compatibleCreations` returns).
