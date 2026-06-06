# M60 — Editor Dark Theme Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the default ImGui grey with a UE5-style deep-charcoal + subtle-blue dark theme across the whole editor, defined as named constants for easy retuning.

**Architecture:** A file-local `applyIronDarkTheme(ImGuiStyle&)` helper + named `static const ImVec4` palette constants in `ImGuiLayer.cpp`, called right after `StyleColorsDark()` (kept as a base). The node-editor canvas (`ed::Style` in `NodeGraphPanel`'s ctor) gets a small color nudge to match. Pure cosmetic — visual-gated, no unit test.

**Tech Stack:** C++17, ImGui 1.92.8, imgui-node-editor (vendored). Build dir `build-vk`, `--config Debug`. Bash tool is bash; prefix commands with `cd "C:/Users/elias/Documents/_dev/iron-core-engine" &&`. Branch `m60-editor-theme` (already created off merged M59). Commit trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` (tmp file + `git commit -F`; never `git add` under `tmp/`).

---

## File Structure

- **`engine/editor/ImGuiLayer.cpp`** (MODIFY) — add the named palette constants + `applyIronDarkTheme(ImGuiStyle&)` in an anonymous namespace; call it after `StyleColorsDark()` in `init()`.
- **`engine/editor/NodeGraphPanel.cpp`** (MODIFY) — small `ed::Style` color nudge in the constructor so the canvas matches the panels.

---

### Task 1: Apply the dark theme

**Files:**
- Modify: `engine/editor/ImGuiLayer.cpp`
- Modify: `engine/editor/NodeGraphPanel.cpp`

No unit test (pure ImGui styling; verified at the visual gate).

- [ ] **Step 1: Add the palette constants + theme helper (anonymous namespace)**

In `engine/editor/ImGuiLayer.cpp`, right after `namespace iron {` (line ~18) and before `bool ImGuiLayer::init(...)`, add:
```cpp
namespace {

// --- Iron editor dark theme (deep charcoal + subtle blue). Tweak here. ---
const ImVec4 kBgWindow     = ImColor( 27,  27,  30, 255);  // #1b1b1e
const ImVec4 kBgChild      = ImColor( 24,  24,  27, 255);  // #18181b
const ImVec4 kBgPopup      = ImColor( 30,  30,  34, 255);  // #1e1e22
const ImVec4 kBgTitle      = ImColor( 36,  36,  40, 255);  // #242428
const ImVec4 kBgPanel      = ImColor( 46,  46,  52, 255);  // #2e2e34
const ImVec4 kFrame        = ImColor( 42,  42,  48, 255);  // #2a2a30
const ImVec4 kFrameHover   = ImColor( 52,  52,  60, 255);  // #34343c
const ImVec4 kFrameActive  = ImColor( 61,  61,  70, 255);  // #3d3d46
const ImVec4 kButton       = ImColor( 46,  46,  54, 255);  // #2e2e36
const ImVec4 kButtonHover  = ImColor( 58,  58,  68, 255);  // #3a3a44
const ImVec4 kAccent       = ImColor( 61, 126, 170, 255);  // #3d7eaa
const ImVec4 kAccentBright = ImColor( 74, 144, 194, 255);  // #4a90c2
const ImVec4 kAccentDim    = ImColor( 61, 126, 170, 110);  // accent @ low alpha
const ImVec4 kText         = ImColor(226, 226, 228, 255);  // #e2e2e4
const ImVec4 kTextDim      = ImColor(110, 110, 118, 255);  // #6e6e76
const ImVec4 kBorder       = ImColor( 58,  58,  66, 255);  // #3a3a42
const ImVec4 kScrollGrab   = ImColor( 58,  58,  68, 255);
const ImVec4 kScrollGrabHv = ImColor( 74,  74,  86, 255);
const ImVec4 kDimBg        = ImColor(  0,   0,   0, 120);  // modal dim

void applyIronDarkTheme(ImGuiStyle& s) {
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = kText;
    c[ImGuiCol_TextDisabled]          = kTextDim;
    c[ImGuiCol_WindowBg]              = kBgWindow;
    c[ImGuiCol_ChildBg]               = kBgChild;
    c[ImGuiCol_PopupBg]               = kBgPopup;
    c[ImGuiCol_Border]                = kBorder;
    c[ImGuiCol_FrameBg]               = kFrame;
    c[ImGuiCol_FrameBgHovered]        = kFrameHover;
    c[ImGuiCol_FrameBgActive]         = kFrameActive;
    c[ImGuiCol_TitleBg]               = kBgWindow;
    c[ImGuiCol_TitleBgActive]         = kBgTitle;
    c[ImGuiCol_TitleBgCollapsed]      = kBgWindow;
    c[ImGuiCol_MenuBarBg]             = kBgTitle;
    c[ImGuiCol_ScrollbarBg]           = kBgWindow;
    c[ImGuiCol_ScrollbarGrab]         = kScrollGrab;
    c[ImGuiCol_ScrollbarGrabHovered]  = kScrollGrabHv;
    c[ImGuiCol_ScrollbarGrabActive]   = kAccent;
    c[ImGuiCol_CheckMark]             = kAccentBright;
    c[ImGuiCol_SliderGrab]            = kAccent;
    c[ImGuiCol_SliderGrabActive]      = kAccentBright;
    c[ImGuiCol_Button]                = kButton;
    c[ImGuiCol_ButtonHovered]         = kButtonHover;
    c[ImGuiCol_ButtonActive]          = kAccent;
    c[ImGuiCol_Header]                = kBgPanel;
    c[ImGuiCol_HeaderHovered]         = kAccentDim;
    c[ImGuiCol_HeaderActive]          = kAccent;
    c[ImGuiCol_Separator]             = kBorder;
    c[ImGuiCol_SeparatorHovered]      = kAccent;
    c[ImGuiCol_SeparatorActive]       = kAccentBright;
    c[ImGuiCol_ResizeGrip]            = kBorder;
    c[ImGuiCol_ResizeGripHovered]     = kAccent;
    c[ImGuiCol_ResizeGripActive]      = kAccentBright;
    c[ImGuiCol_Tab]                   = kBgTitle;
    c[ImGuiCol_TabHovered]            = kAccentBright;
    c[ImGuiCol_TabSelected]           = kBgPanel;
    c[ImGuiCol_TabSelectedOverline]   = kAccent;
    c[ImGuiCol_TabDimmed]             = kBgWindow;
    c[ImGuiCol_TabDimmedSelected]     = kBgTitle;
    c[ImGuiCol_DockingPreview]        = kAccentDim;
    c[ImGuiCol_DockingEmptyBg]        = kBgChild;
    c[ImGuiCol_TextSelectedBg]        = kAccentDim;
    c[ImGuiCol_DragDropTarget]        = kAccentBright;
    c[ImGuiCol_NavCursor]             = kAccent;   // (was ImGuiCol_NavHighlight pre-1.91.4)
    c[ImGuiCol_ModalWindowDimBg]      = kDimBg;

    // Restrained modern rounding + thin borders.
    s.WindowRounding    = 4.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 4.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 4.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBorderSize     = 0.0f;
    s.FramePadding      = ImVec2(8.0f, 4.0f);
    s.ItemSpacing       = ImVec2(8.0f, 5.0f);
}

}  // namespace
```
Note: `ImColor(int,int,int,int)` implicitly converts to `ImVec4` (via `.Value`); `ImColor` + `ImVec4` + all `ImGuiCol_*`/`ImGuiStyle` come from the already-included `<imgui.h>`. If the compiler rejects the implicit `ImColor`→`ImVec4` in the initializer, append `.Value` to each (e.g. `ImColor(27,27,30,255).Value`).

- [ ] **Step 2: Call the theme in `init()`**

In `ImGuiLayer.cpp`, in `init()`, change line ~51:
```cpp
    ImGui::StyleColorsDark();
```
to:
```cpp
    ImGui::StyleColorsDark();              // sane base; we override the key colors below
    applyIronDarkTheme(ImGui::GetStyle());
```

- [ ] **Step 3: Nudge the node-editor canvas to match (NodeGraphPanel ctor)**

In `engine/editor/NodeGraphPanel.cpp`, the constructor sets `ed::Style`. Align its dark colors to the theme family (charcoal canvas + a card that lifts just above the panel charcoal + light border). Replace the existing color lines:
```cpp
    st.Colors[ed::StyleColor_Bg]         = ImColor(24, 24, 28, 255);
    st.Colors[ed::StyleColor_Grid]       = ImColor(255, 255, 255, 16);
    st.Colors[ed::StyleColor_NodeBg]     = ImColor(43, 43, 50, 255);
    st.Colors[ed::StyleColor_NodeBorder] = ImColor(255, 255, 255, 70);
```
with:
```cpp
    st.Colors[ed::StyleColor_Bg]         = ImColor(24, 24, 27, 255);   // = theme kBgChild
    st.Colors[ed::StyleColor_Grid]       = ImColor(255, 255, 255, 14); // subtle grid
    st.Colors[ed::StyleColor_NodeBg]     = ImColor(48, 48, 56, 255);   // lifts above panel charcoal
    st.Colors[ed::StyleColor_NodeBorder] = ImColor(255, 255, 255, 64); // light, slightly softer
```
(Keep `NodeRounding`/`NodeBorderWidth` and the per-category header colors unchanged.)

- [ ] **Step 4: Build**

Run:
```bash
cd "C:/Users/elias/Documents/_dev/iron-core-engine" && cmake --build build-vk --target sandbox --config Debug
```
Expected: clean build (pre-existing LNK4217 glfw/imgui import warnings are OK). All `ImGuiCol_*` names used here are verified against the vendored `<imgui.h>` (Tab names `Tab`/`TabHovered`/`TabSelected`/`TabSelectedOverline`/`TabDimmed`/`TabDimmedSelected`; `NavCursor`; `DockingEmptyBg`; `DockingPreview`).

- [ ] **Step 5: Commit**

Stage `engine/editor/ImGuiLayer.cpp` and `engine/editor/NodeGraphPanel.cpp`. Subject: `M60: UE5-style dark editor theme (deep charcoal + subtle blue)`. tmp file + `git commit -F`; `rm -f` after.

---

### Task 2: Build, visual gate, PR

**Files:** none.

- [ ] **Step 1: Clean full build** — `cmake --build build-vk --config Debug` (all targets). Expected: builds.
- [ ] **Step 2: Test sweep** — `ctest --test-dir build-vk -C Debug --output-on-failure`. Expected: 74/74 (the theme touches no tested code; this just confirms nothing broke). Record N/N.
- [ ] **Step 3: Visual gate** — launch `build-vk/games/11-sandbox/Debug/sandbox.exe`: panels/title bars/menus are deep charcoal; buttons/tabs/sliders/checkmarks/selection show the subtle blue accent on hover/active; text crisp + readable; the node-editor canvas matches the panels; nothing unreadably low-contrast. Tune the named constants at the gate if needed (one-line edits).
- [ ] **Step 4: PR** — push, open PR (base main, `🤖 Generated with [Claude Code]` footer), background CI-watch-merge (squash; re-run on a vcpkg-504 flake). Update memory (`iron-core-engine-progress.md` + `MEMORY.md` LATEST).

---

## Self-Review

**Spec coverage:**
- Named static palette constants → Task 1 Step 1.
- `applyIronDarkTheme` after `StyleColorsDark` → Task 1 Steps 1–2.
- Full `ImGuiCol_` mapping (incl. verified Tab names) + style vars → Task 1 Step 1.
- Node-editor canvas harmony → Task 1 Step 3.
- Visual-gated, no unit test → Task 2.
- No runtime theme switching (YAGNI) → not built.

**Placeholder scan:** No TBDs. Every color/var is concrete. The two compile-fallbacks (`.Value` suffix; drop `NavHighlight` if renamed) are explicit conditional instructions, not placeholders.

**Type consistency:** All constants are `const ImVec4` assigned to `ImGuiStyle::Colors[]` (ImVec4) and the style-var members (floats / ImVec2) match their types. `applyIronDarkTheme(ImGuiStyle&)` is defined once and called once. The node-editor `ed::StyleColor_*` names match M58/M59 usage.
