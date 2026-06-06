# M58 — Node Editor Look v2 + Group Nodes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Node Editor the imgui-node-editor UE4-blueprint look — filled colored header bands + white titles + tuned node style, a crisp TTF font, and resizable comment/group regions.

**Architecture:** Pure presentation for A (header bands) and B (font); C adds an editor-only `Comment` model to `GraphEditorModel` serialized under a sibling `"comments"` key that the executable graph loader (`iron::fromJson`) ignores. The headless `Graph` + `GraphEvaluator` are untouched, and M57 undo/redo covers comments for free (it snapshots `graphModel.toJson()`).

**Tech Stack:** C++17, ImGui 1.92.8, thedmd/imgui-node-editor (vendored), nlohmann/json, CTest. Build dir `build-vk` (Vulkan), `--config Debug`, `ctest -C Debug`. Bash tool is bash (not PowerShell); prefix commands with `cd "C:/Users/elias/Documents/_dev/iron-core-engine" &&`. Branch: `m58-node-editor-look` (already created off merged M57). Every commit ends with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` (use a tmp file + `git commit -F`; never `git add` under `tmp/`).

---

## File Structure

- **`games/11-sandbox/assets/fonts/Roboto-Medium.ttf`** (NEW) — vendored UI font (Apache-2.0). Copied next to the exe by the sandbox's existing `copy_directory assets` POST_BUILD step (no CMake change needed).
- **`engine/editor/ImGuiLayer.cpp`** (MODIFY) — load the TTF at init with a graceful fallback.
- **`engine/editor/NodeGraphPanel.cpp`** (MODIFY) — `ed::Style` tuning in the ctor; white titles + filled header bands in the node loop; comment rendering/add/delete/persist.
- **`engine/nodes/GraphEditorModel.h` / `.cpp`** (MODIFY) — `Comment` struct, `comments_` + `nextCommentId_`, add/delete/setRect/setTitle, comment serialization in `toJson`/`loadFromJson`.
- **`tests/test_graph_editor.cpp`** (MODIFY) — comment unit tests + a runtime-tolerance assertion.

---

### Task 1: Crisp editor font (B)

**Files:**
- Create: `games/11-sandbox/assets/fonts/Roboto-Medium.ttf`
- Modify: `engine/editor/ImGuiLayer.cpp`

- [ ] **Step 1: Vendor the font**

Download ImGui's bundled Roboto-Medium (Apache-2.0) into the sandbox assets:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && mkdir -p games/11-sandbox/assets/fonts && curl -L -o games/11-sandbox/assets/fonts/Roboto-Medium.ttf https://github.com/ocornut/imgui/raw/master/misc/fonts/Roboto-Medium.ttf
```
Verify it's a real TTF (~160KB, not an HTML error page):
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && ls -l games/11-sandbox/assets/fonts/Roboto-Medium.ttf && file games/11-sandbox/assets/fonts/Roboto-Medium.ttf
```
Expected: ~160-170KB, `TrueType Font data`. If the download fails or yields HTML/0 bytes, STOP and report BLOCKED (the controller will supply the font another way).

- [ ] **Step 2: Load the font in `ImGuiLayer::init` with a fallback**

In `engine/editor/ImGuiLayer.cpp`, add the include near the top (with the other engine includes):
```cpp
#include "core/Platform.h"   // iron::executableDir()
```
Then in `ImGuiLayer::init`, immediately after `ImGui::StyleColorsDark();` (currently line ~48) and before `ImGui_ImplGlfw_InitForVulkan(...)`, insert:
```cpp
    // M58: crisp UI font (Roboto-Medium, Apache-2.0), copied next to the exe with
    // the rest of the assets. Fallback to the built-in bitmap font if it's missing
    // so the editor never fails to start.
    {
        ImGuiIO& io = ImGui::GetIO();
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        const std::string fontPath = executableDir() + "/assets/fonts/Roboto-Medium.ttf";
        ImFont* f = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, &cfg);
        if (!f) {
            io.Fonts->AddFontDefault();
            Log::warn("ImGuiLayer: font '%s' not found; using default", fontPath.c_str());
        }
    }
```
(`Log` and `<string>` are already in use in this file; `executableDir()` resolves via the `iron` namespace this file is in.)

- [ ] **Step 3: Build**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target sandbox --config Debug
```
Expected: clean build; the POST_BUILD step copies `assets/` (including `fonts/`) next to `sandbox.exe`.

- [ ] **Step 4: Commit**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && git add games/11-sandbox/assets/fonts/Roboto-Medium.ttf engine/editor/ImGuiLayer.cpp && git commit -F tmp/msg.txt
```
(Subject: `M58: crisp Roboto-Medium editor font with fallback`. Write the message + trailer to `tmp/msg.txt` first, `rm -f` after. Note: if `.gitattributes` routes `*.ttf` through git LFS, the add is handled transparently.)

---

### Task 2: Blueprint node look — header bands + white titles + style (A)

**Files:**
- Modify: `engine/editor/NodeGraphPanel.cpp`

No unit test (pure rendering); verified at the visual gate.

- [ ] **Step 1: Tune `ed::Style` in the panel constructor**

In `engine/editor/NodeGraphPanel.cpp`, replace the constructor:
```cpp
NodeGraphPanel::NodeGraphPanel() { ctx_ = ed::CreateEditor(); }
```
with:
```cpp
NodeGraphPanel::NodeGraphPanel() {
    ctx_ = ed::CreateEditor();
    // M58: soften the node cards to match the blueprint example.
    ed::SetCurrentEditor(ctx_);
    ed::Style& st = ed::GetStyle();
    st.NodeRounding    = 6.0f;
    st.NodeBorderWidth = 1.0f;
    st.Colors[ed::StyleColor_NodeBg]     = ImColor(40, 40, 48, 255);
    st.Colors[ed::StyleColor_NodeBorder] = ImColor(0, 0, 0, 160);
    ed::SetCurrentEditor(nullptr);
}
```

- [ ] **Step 2: White title + capture content bounds, then draw the header band**

In the node draw loop (currently the `for (const Node& n : model.graph().nodes())` block), make these changes:

(a) Wrap the node content in an ImGui group and render the title in **white**, capturing the title bottom. Replace the current header lines:
```cpp
        ed::BeginNode(ed::NodeId(static_cast<std::uintptr_t>(n.id)));
        ImGui::PushID(static_cast<int>(n.id));

        // Header: colored title row (per-category tint).
        ImGui::TextColored(headerColor(t->category).Value, "%s", t->typeName.c_str());
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
```
with:
```cpp
        ed::BeginNode(ed::NodeId(static_cast<std::uintptr_t>(n.id)));
        ImGui::PushID(static_cast<int>(n.id));
        ImGui::BeginGroup();   // M58: wrap content for reliable screen-space bounds

        // Header: white title; the colored band is drawn behind it after EndNode.
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", t->typeName.c_str());
        const float titleBottom = ImGui::GetItemRectMax().y;
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
```

(b) Close the group and capture bounds, then draw the band. Replace the node-loop tail:
```cpp
        ImGui::PopID();
        ed::EndNode();
    }
```
with:
```cpp
        ImGui::EndGroup();   // M58: close the content group
        const ImVec2 contentMin = ImGui::GetItemRectMin();
        const ImVec2 contentMax = ImGui::GetItemRectMax();
        ImGui::PopID();
        ed::EndNode();

        // M58: filled category-colored header band behind the white title.
        if (ImDrawList* bg = ed::GetNodeBackgroundDrawList(
                ed::NodeId(static_cast<std::uintptr_t>(n.id)))) {
            const ImVec4 pad      = ed::GetStyle().NodePadding;   // x=left y=top z=right w=bottom
            const float  rounding = ed::GetStyle().NodeRounding;
            const ImVec2 a(contentMin.x - pad.x, contentMin.y - pad.y);
            const ImVec2 b(contentMax.x + pad.z, titleBottom + 2.0f);
            bg->AddRectFilled(a, b, headerColor(t->category), rounding, ImDrawFlags_RoundCornersTop);
            bg->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), ImColor(0, 0, 0, 90), 1.0f);
        }
    }
```
Notes: `ed::GetNodeBackgroundDrawList` draws in **screen space** and renders on top of the node's background fill but under the content, so the white title sits on the band. `ImGui::BeginGroup`/`EndGroup` is transparent to layout (pins remain individual items) but gives the union screen rect of all content for the band width.

- [ ] **Step 3: Build**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target sandbox --config Debug
```
Expected: clean build. (`ImDrawFlags_RoundCornersTop`, `ImColor`, `ed::GetStyle` are all available via the existing `<imgui.h>` / `<imgui_node_editor.h>` includes.)

- [ ] **Step 4: Commit**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && git add engine/editor/NodeGraphPanel.cpp && git commit -F tmp/msg.txt
```
(Subject: `M58: blueprint header bands + white titles + node style`.)

---

### Task 3: Comment model + serialization (C, part 1)

**Files:**
- Modify: `engine/nodes/GraphEditorModel.h`
- Modify: `engine/nodes/GraphEditorModel.cpp`
- Test: `tests/test_graph_editor.cpp`

- [ ] **Step 1: Declare the `Comment` type, storage, and ops in the header**

In `engine/nodes/GraphEditorModel.h`, add the struct just before `class GraphEditorModel` (inside `namespace iron`):
```cpp
// Editor-only annotation: a movable/resizable labeled backdrop region grouping
// nodes visually. NOT part of the executable Graph — the evaluator never sees it.
struct Comment {
    std::uint32_t id = 0;
    float x = 0.0f, y = 0.0f, w = 240.0f, h = 160.0f;
    std::string title = "Comment";
};
```
Add the public methods (after `setNodePosition`):
```cpp
    // M58: comment/group regions (editor-only; serialized under "comments").
    std::uint32_t addComment(float x, float y, float w, float h, std::string title);
    void deleteComment(std::uint32_t id);
    void setCommentRect(std::uint32_t id, float x, float y, float w, float h);
    void setCommentTitle(std::uint32_t id, std::string title);
    const std::vector<Comment>& comments() const { return comments_; }
```
Add the private members (after `bool dirty_`):
```cpp
    std::vector<Comment> comments_;
    std::uint32_t nextCommentId_ = 1;
```
(`<cstdint>`, `<vector>`, `<string>` come in via the included `nodes/NodeGraph.h`.)

- [ ] **Step 2: Write the failing tests**

In `tests/test_graph_editor.cpp`, add the include near the top (with the other `nodes/` includes):
```cpp
#include "nodes/NodeGraphIO.h"   // iron::fromJson (runtime-tolerance check)
```
Add these blocks inside `main()` (before the final `return iron_test_result();`):
```cpp
    // M58: comments — add / mutate / delete + dirty + JSON round-trip.
    {
        GraphEditorModel m(&reg);
        const std::uint32_t id = m.addComment(10, 20, 200, 100, "Update UI");
        CHECK(m.comments().size() == 1u);
        CHECK(m.comments()[0].id == id);
        CHECK_NEAR(m.comments()[0].x, 10.0f);
        CHECK(m.comments()[0].title == "Update UI");
        CHECK(m.dirty());

        m.clearDirty();
        m.setCommentRect(id, 11, 22, 210, 110);
        CHECK(m.dirty());
        CHECK_NEAR(m.comments()[0].w, 210.0f);

        m.clearDirty();
        m.setCommentTitle(id, "Renamed");
        CHECK(m.dirty());
        CHECK(m.comments()[0].title == "Renamed");

        GraphEditorModel m2(&reg);
        CHECK(m2.loadFromJson(m.toJson()));
        CHECK(m2.comments().size() == 1u);
        CHECK(m2.comments()[0].id == id);
        CHECK(m2.comments()[0].title == "Renamed");
        CHECK_NEAR(m2.comments()[0].h, 110.0f);

        m.clearDirty();
        m.deleteComment(id);
        CHECK(m.comments().empty());
        CHECK(m.dirty());
    }

    // M58: runtime tolerance — the executable loader ignores "comments".
    {
        GraphEditorModel m(&reg);
        m.addNode("Entry", 0, 0);
        m.addComment(0, 0, 100, 100, "note");
        const nlohmann::json j = m.toJson();
        CHECK(j.contains("comments"));
        auto g = iron::fromJson(j, reg);   // the Play-mode path
        CHECK(g.has_value());              // comments don't break executable load
    }
```
(`reg` is the `NodeRegistry` already constructed at the top of the test's `main()`. `nlohmann::json` is available via `GraphEditorModel.h`.)

- [ ] **Step 3: Run the test to verify it fails**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target test_graph_editor --config Debug
```
Expected: FAIL — compile error: `addComment`/`comments`/etc. are not members of `GraphEditorModel`.

- [ ] **Step 4: Implement the ops + serialization**

In `engine/nodes/GraphEditorModel.cpp`, ensure these includes are present near the top (add any missing):
```cpp
#include <algorithm>   // std::remove_if, std::max
#include <utility>     // std::move
```
Add the comment ops (e.g. after `setNodePosition`'s definition):
```cpp
std::uint32_t GraphEditorModel::addComment(float x, float y, float w, float h, std::string title) {
    Comment c;
    c.id = nextCommentId_++;
    c.x = x; c.y = y; c.w = w; c.h = h;
    c.title = std::move(title);
    comments_.push_back(std::move(c));
    dirty_ = true;
    return comments_.back().id;
}

void GraphEditorModel::deleteComment(std::uint32_t id) {
    const auto it = std::remove_if(comments_.begin(), comments_.end(),
                                   [id](const Comment& c) { return c.id == id; });
    if (it != comments_.end()) { comments_.erase(it, comments_.end()); dirty_ = true; }
}

void GraphEditorModel::setCommentRect(std::uint32_t id, float x, float y, float w, float h) {
    for (Comment& c : comments_) {
        if (c.id != id) continue;
        if (c.x != x || c.y != y || c.w != w || c.h != h) {
            c.x = x; c.y = y; c.w = w; c.h = h;
            dirty_ = true;
        }
        return;
    }
}

void GraphEditorModel::setCommentTitle(std::uint32_t id, std::string title) {
    for (Comment& c : comments_) {
        if (c.id != id) continue;
        if (c.title != title) { c.title = std::move(title); dirty_ = true; }
        return;
    }
}
```
Replace `toJson`:
```cpp
nlohmann::json GraphEditorModel::toJson() const { return iron::toJson(graph_); }
```
with:
```cpp
nlohmann::json GraphEditorModel::toJson() const {
    nlohmann::json j = iron::toJson(graph_);
    nlohmann::json arr = nlohmann::json::array();
    for (const Comment& c : comments_) {
        arr.push_back({{"id", c.id}, {"x", c.x}, {"y", c.y},
                       {"w", c.w}, {"h", c.h}, {"title", c.title}});
    }
    j["comments"]      = arr;
    j["nextCommentId"] = nextCommentId_;
    return j;
}
```
Replace `loadFromJson`:
```cpp
bool GraphEditorModel::loadFromJson(const nlohmann::json& j) {
    if (!registry_) return false;
    auto g = iron::fromJson(j, *registry_);
    if (!g) return false;
    graph_ = std::move(*g);
    selected_ = 0;
    dirty_ = false;
    return true;
}
```
with:
```cpp
bool GraphEditorModel::loadFromJson(const nlohmann::json& j) {
    if (!registry_) return false;
    auto g = iron::fromJson(j, *registry_);
    if (!g) return false;
    graph_ = std::move(*g);
    selected_ = 0;

    comments_.clear();
    nextCommentId_ = 1;
    if (j.contains("comments") && j["comments"].is_array()) {
        for (const auto& jc : j["comments"]) {
            Comment c;
            c.id    = jc.value("id", 0u);
            c.x     = jc.value("x", 0.0f);
            c.y     = jc.value("y", 0.0f);
            c.w     = jc.value("w", 240.0f);
            c.h     = jc.value("h", 160.0f);
            c.title = jc.value("title", std::string("Comment"));
            if (c.id == 0) continue;   // skip malformed
            comments_.push_back(std::move(c));
            nextCommentId_ = std::max(nextCommentId_, c.id + 1);
        }
    }
    nextCommentId_ = std::max(nextCommentId_, j.value("nextCommentId", 1u));

    dirty_ = false;
    return true;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target test_graph_editor --config Debug && ctest --test-dir build-vk -C Debug -R test_graph_editor --output-on-failure
```
Expected: PASS — including the new comment + runtime-tolerance blocks.

- [ ] **Step 6: Commit**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && git add engine/nodes/GraphEditorModel.h engine/nodes/GraphEditorModel.cpp tests/test_graph_editor.cpp && git commit -F tmp/msg.txt
```
(Subject: `M58: comment/group model + serialization (editor-only, runtime-tolerant)`.)

---

### Task 4: Comment rendering (C, part 2)

**Files:**
- Modify: `engine/editor/NodeGraphPanel.h`
- Modify: `engine/editor/NodeGraphPanel.cpp`

No unit test (rendering); verified at the visual gate.

- [ ] **Step 1: Add per-comment editor-state maps to the header**

In `engine/editor/NodeGraphPanel.h`, add `#include <unordered_map>` and `#include <cstdint>` to the includes (the file already has `#include <unordered_set>`). The header must stay ImGui-free, so store the size offset as two `float` maps (not `ImVec2`). Add private members after `placed_`:
```cpp
    std::unordered_set<std::uint32_t> placedComments_;                    // comment id already positioned
    std::unordered_map<std::uint32_t, float> commentOffX_, commentOffY_;  // node-vs-group size delta, measured once
```

- [ ] **Step 2: Add a "Comment" button to the toolbar**

In `engine/editor/NodeGraphPanel.cpp`, in the palette area (right after the `for (const NodeTypeDesc* t : model.registry()->all())` palette loop closes, inside the `if (model.registry())` block), add:
```cpp
        ImGui::SameLine();
        if (ImGui::SmallButton("+ Comment")) {
            model.addComment(spawnX_, spawnY_, 240.0f, 160.0f, "Comment");
            spawnX_ += 30.0f; spawnY_ += 30.0f;
            if (spawnX_ > 400.0f) { spawnX_ = 40.0f; spawnY_ = 40.0f; }
        }
```

- [ ] **Step 3: Render comments as resizable groups (before the node loop, so they sit behind)**

In `NodeGraphPanel.cpp`, immediately after `ed::Begin("canvas");` and **before** the `for (const Node& n : ...)` node loop, add a helper and the comment loop:
```cpp
    // M58: comment editor-ids live in a high-bit namespace so they never collide
    // with node ids, pin ids (node<<8|...), or link ids (i+1).
    auto commentEd = [](std::uint32_t id) -> std::uintptr_t {
        return (std::uintptr_t{1} << 62) | id;
    };

    for (const Comment& c : model.comments()) {
        const ed::NodeId cid(commentEd(c.id));
        if (placedComments_.find(c.id) == placedComments_.end()) {
            ed::SetNodePosition(cid, ImVec2(c.x, c.y));
            placedComments_.insert(c.id);
        }
        ed::PushStyleColor(ed::StyleColor_NodeBg,     ImColor(60, 60, 75, 80).Value);
        ed::PushStyleColor(ed::StyleColor_NodeBorder, ImColor(130, 130, 160, 180).Value);
        ed::BeginNode(cid);
        ImGui::PushID(static_cast<int>(c.id) ^ 0x4000);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", c.title.c_str());
        ImGui::SetNextItemWidth(c.w > 48.0f ? c.w - 16.0f : 96.0f);
        if (ImGui::InputText("##ctitle", buf, sizeof(buf)))
            model.setCommentTitle(c.id, buf);
        ed::Group(ImVec2(c.w, c.h));
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleColor(2);
    }
```

- [ ] **Step 4: Persist comment move/resize (after the node-position sync loop)**

In `NodeGraphPanel.cpp`, right after the existing M56 node-position sync loop (`for (const Node& n : model.graph().nodes()) { ... model.setNodePosition ... }`), add the comment persistence. The node's reported size includes the title row + padding, so we measure that delta once per comment and subtract it, keeping `ed::Group(c.w,c.h)` stable across frames:
```cpp
    // M58: persist comment move/resize. GetNodeSize includes the title row +
    // padding, so subtract a once-measured offset to recover the group size.
    for (const Comment& c : model.comments()) {
        const ed::NodeId cid(commentEd(c.id));
        const ImVec2 pos = ed::GetNodePosition(cid);
        const ImVec2 sz  = ed::GetNodeSize(cid);
        if (commentOffX_.find(c.id) == commentOffX_.end()) {
            // First measured frame: node size - requested group size = overhead.
            commentOffX_[c.id] = sz.x - c.w;
            commentOffY_[c.id] = sz.y - c.h;
        }
        const float w = sz.x - commentOffX_[c.id];
        const float h = sz.y - commentOffY_[c.id];
        if (pos.x != c.x || pos.y != c.y || w != c.w || h != c.h)
            model.setCommentRect(c.id, pos.x, pos.y,
                                 w > 32.0f ? w : 32.0f, h > 32.0f ? h : 32.0f);
    }
```

- [ ] **Step 5: Handle comment deletion in the existing delete block**

In `NodeGraphPanel.cpp`, in the `ed::BeginDelete()` block, the `while (ed::QueryDeletedNode(&nid))` loop currently calls `model.deleteNode(...)`. Route comment ids to `deleteComment`. Replace:
```cpp
        ed::NodeId nid;
        while (ed::QueryDeletedNode(&nid)) {
            if (ed::AcceptDeletedItem()) model.deleteNode(static_cast<NodeId>(nid.Get()));
        }
```
with:
```cpp
        ed::NodeId nid;
        while (ed::QueryDeletedNode(&nid)) {
            if (ed::AcceptDeletedItem()) {
                const std::uintptr_t raw = nid.Get();
                if (raw & (std::uintptr_t{1} << 62)) {
                    model.deleteComment(static_cast<std::uint32_t>(raw & 0xFFFFFFFFu));
                    placedComments_.erase(static_cast<std::uint32_t>(raw & 0xFFFFFFFFu));
                } else {
                    model.deleteNode(static_cast<NodeId>(raw));
                }
            }
        }
```

- [ ] **Step 6: Build**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target sandbox --config Debug
```
Expected: clean build. (`<cstdio>` for `snprintf` is already included in the file.)

- [ ] **Step 7: Commit**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && git add engine/editor/NodeGraphPanel.h engine/editor/NodeGraphPanel.cpp && git commit -F tmp/msg.txt
```
(Subject: `M58: render resizable comment/group regions in the node editor`.)

---

### Task 5: Full build, test sweep, and visual gate

**Files:** none (verification).

- [ ] **Step 1: Clean full build of all targets**

Per [[verify-clean-build-before-ci]]:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --config Debug
```
Expected: all targets build.

- [ ] **Step 2: Full test suite**

```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: all pass (incl. `test_graph_editor` with the new comment blocks). Record the N/N count. Do NOT proceed if anything fails.

- [ ] **Step 3: Code review**

Per [[always-code-review-changes]], review `git diff main...HEAD` — focus on the header-band geometry (screen-space rects), the comment id-namespacing + size-offset persistence (no per-frame growth/drift), and that the runtime path is unaffected.

- [ ] **Step 4: Hand off to the user's visual gate**

Launch the sandbox and verify by hand:
- Text across the whole editor is crisp (font), not blocky.
- Nodes show filled category-colored header bands with white titles (the blueprint look); rounded cards.
- A "+ Comment" button adds a labeled box; typing renames it; dragging moves it; the corner handle resizes it without it creeping/growing; nodes render on top of it.
- Comments persist through Save→Load and through Ctrl+Z/Ctrl+Y (undo/redo).
- Play still works (an entity with a logic graph that has comments still runs).

- [ ] **Step 5: After the gate passes — PR**

Push the branch, open a PR (base main, body ending with the `🤖 Generated with [Claude Code]` line), and start a background CI-watch-and-merge (squash). Then update memory (`iron-core-engine-progress.md` + `MEMORY.md` LATEST marker) with the M58 entry.

---

## Self-Review

**Spec coverage:**
- A (header bands + white titles + `ed::Style`) → Task 2.
- B (crisp TTF + fallback) → Task 1.
- C model + serialization (Comment, ops, `toJson`/`loadFromJson`, dedicated `nextCommentId_`, runtime-ignored `"comments"`) → Task 3.
- C rendering (resizable `ed::Group`, add button, editable title, id namespacing, move/resize persist, delete routing) → Task 4.
- Headless tests (comment add/mutate/delete/dirty + round-trip + runtime tolerance) → Task 3 Step 2.
- Visual gate for A/B/C → Task 5 Step 4.
- Undo coverage (comments ride in `toJson`; ops set `dirty_`) → Task 3 ops all set `dirty_`; M57 graph coalescing keys off `dirty()`.

**Placeholder scan:** No TBDs; every code step is complete. `ed::Style` exact values are concrete (tunable at the gate, not placeholders).

**Type consistency:** `Comment{id,x,y,w,h,title}` fields match across header (Task 3.1), tests (3.2), impl (3.4), and rendering (Task 4). `addComment/deleteComment/setCommentRect/setCommentTitle/comments()` signatures consistent. `commentEd(id)` (bit-62 namespace) used identically in render (4.3), persist (4.4), delete (4.5). `nextCommentId_` / `placedComments_` / `commentOffX_`/`commentOffY_` each defined once and used consistently. `headerColor` reused as a fill in Task 2 (no new palette).

**Known risk carried in the spec:** header-band geometry uses ImGui screen-space rects (group bounds + `NodePadding`) — validated at the gate; the comment size-offset is measured once per comment to prevent resize drift — also gate-validated. Both have simple fallbacks (fixed band width / non-resizable comments) if they misbehave.
