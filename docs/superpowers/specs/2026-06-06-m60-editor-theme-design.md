# M60 — Editor Dark Theme (UE5-style) — Design

> **Editor polish milestone.** Driven by user feedback after M59: the editor's default ImGui grey is "boring"; the user wants a darker, UE5-style look. A single cohesive built-in dark theme — deep charcoal backgrounds + a restrained subtle-blue accent — applied globally.

## Goal

Replace the flat default `StyleColorsDark` look with a deep-charcoal, subtle-blue (UE5-ish) palette across the whole editor, defined as **named color constants** in one place so it's trivial to retune later. The node-editor canvas (its own `ed::Style`) is nudged to match so the editor reads as one theme.

## Scope (locked in brainstorming)

In: a custom global ImGui palette + style-vars helper; named static color constants; node-editor canvas/node color harmony. Out (YAGNI): runtime theme switching, multiple selectable themes, a settings UI, per-user persistence — just one good built-in dark theme. Accent = subtle blue `#3d7eaa`; base = deep charcoal (UE5-ish).

## Architecture

One self-contained styling change, no public API change. The only global style call today is `ImGui::StyleColorsDark()` at `ImGuiLayer.cpp:51` (confirmed: no other file sets ImGui colors). Keep that call as a sane base, then apply a file-local helper `applyIronDarkTheme(ImGuiStyle&)` immediately after it to override the key colors + style vars. Any `ImGuiCol_` not overridden retains a reasonable dark default.

The palette is defined as **named `static const ImVec4` constants** (built via `ImColor(r,g,b,a)`) in an anonymous namespace at the top of `ImGuiLayer.cpp`, grouped and commented with their hex, so retuning is a one-line edit per color. (User request: "keep it as statics so we can change later.")

The node-editor canvas already sets its own dark `ed::Style` in `NodeGraphPanel`'s constructor (M58/M59: canvas Bg `(24,24,28)`, node card `(43,43,50)`, light border). Those are nudged to reference the same palette family so the canvas matches the panels.

## Palette (named constants)

Defined once (anonymous namespace in `ImGuiLayer.cpp`):

```cpp
namespace {
// --- Iron editor dark theme (deep charcoal + subtle blue). Tweak here. ---
const ImVec4 kBgWindow     = ImColor( 27,  27,  30, 255);  // #1b1b1e  window background
const ImVec4 kBgChild      = ImColor( 24,  24,  27, 255);  // #18181b  child regions
const ImVec4 kBgPopup      = ImColor( 30,  30,  34, 255);  // #1e1e22  popups/menus
const ImVec4 kBgTitle      = ImColor( 36,  36,  40, 255);  // #242428  title/menubar/tab-inactive
const ImVec4 kBgPanel      = ImColor( 46,  46,  52, 255);  // #2e2e34  headers/tab-active
const ImVec4 kFrame        = ImColor( 42,  42,  48, 255);  // #2a2a30  inputs/sliders/combos
const ImVec4 kFrameHover   = ImColor( 52,  52,  60, 255);  // #34343c
const ImVec4 kFrameActive  = ImColor( 61,  61,  70, 255);  // #3d3d46
const ImVec4 kButton       = ImColor( 46,  46,  54, 255);  // #2e2e36
const ImVec4 kButtonHover  = ImColor( 58,  58,  68, 255);  // #3a3a44
const ImVec4 kAccent       = ImColor( 61, 126, 170, 255);  // #3d7eaa  subtle blue
const ImVec4 kAccentBright = ImColor( 74, 144, 194, 255);  // #4a90c2  hover/active
const ImVec4 kAccentDim    = ImColor( 61, 126, 170, 110);  // accent @ low alpha (hover headers)
const ImVec4 kText         = ImColor(226, 226, 228, 255);  // #e2e2e4
const ImVec4 kTextDim      = ImColor(110, 110, 118, 255);  // #6e6e76  disabled
const ImVec4 kBorder       = ImColor( 58,  58,  66, 255);  // #3a3a42  thin borders/separators
const ImVec4 kScrollGrab   = ImColor( 58,  58,  68, 255);  // scrollbar grab
const ImVec4 kScrollGrabHv = ImColor( 74,  74,  86, 255);
}  // namespace
```

### Color assignments (`ImGuiCol_` → constant)
- `WindowBg`=kBgWindow · `ChildBg`=kBgChild · `PopupBg`=kBgPopup · `MenuBarBg`=kBgTitle
- `TitleBg`=kBgWindow · `TitleBgActive`=kBgTitle · `TitleBgCollapsed`=kBgWindow
- `Border`=kBorder · `Separator`=kBorder · `SeparatorHovered`=kAccent · `SeparatorActive`=kAccentBright
- `FrameBg`=kFrame · `FrameBgHovered`=kFrameHover · `FrameBgActive`=kFrameActive
- `Button`=kButton · `ButtonHovered`=kButtonHover · `ButtonActive`=kAccent
- `Header`=kBgPanel · `HeaderHovered`=kAccentDim · `HeaderActive`=kAccent
- Tabs (ImGui 1.92.8 names — verified in the vendored header): `ImGuiCol_Tab`=kBgTitle · `ImGuiCol_TabHovered`=kAccentBright · `ImGuiCol_TabSelected`=kBgPanel · `ImGuiCol_TabSelectedOverline`=kAccent (the selected-tab highlight line) · `ImGuiCol_TabDimmed`=kBgWindow · `ImGuiCol_TabDimmedSelected`=kBgTitle
- `CheckMark`=kAccentBright · `SliderGrab`=kAccent · `SliderGrabActive`=kAccentBright
- `Text`=kText · `TextDisabled`=kTextDim · `TextSelectedBg`=kAccentDim
- `ScrollbarBg`=kBgWindow · `ScrollbarGrab`=kScrollGrab · `ScrollbarGrabHovered`=kScrollGrabHv · `ScrollbarGrabActive`=kAccent
- `ResizeGrip`=kBorder · `ResizeGripHovered`=kAccent · `ResizeGripActive`=kAccentBright
- `DockingPreview`=kAccentDim · `DragDropTarget`=kAccentBright
- `PopupBg`=kBgPopup · `ModalWindowDimBg`= a dim black (`ImColor(0,0,0,120)`)
(Any `ImGuiCol_` not listed keeps the `StyleColorsDark` base value.)

### Style vars
`WindowRounding=4`, `ChildRounding=4`, `FrameRounding=4`, `GrabRounding=3`, `TabRounding=4`, `PopupRounding=4`, `ScrollbarRounding=4`; `WindowBorderSize=1`, `FrameBorderSize=0`, `TabBorderSize=0`; `FramePadding=(8,4)`, `ItemSpacing=(8,5)`. Restrained — crisp, not bubbly.

## Node-editor canvas harmony
In `NodeGraphPanel`'s constructor, keep the existing `ed::Style` structure but align values to the palette: canvas `StyleColor_Bg` ≈ kBgChild `(24,24,27)`, node `StyleColor_NodeBg` ≈ a touch above kBgPanel so cards lift off the canvas, `StyleColor_NodeBorder` ≈ a light translucent border. (These are already close from M58/M59; this is a small nudge for exact harmony, not a rewrite. The per-category header-band colors are unchanged.)

## Error handling
None meaningful — setting style colors/vars cannot fail. The theme is applied once at `init()`; if `applyIronDarkTheme` were somehow skipped, the `StyleColorsDark` base still yields a usable dark UI.

## Testing
Pure cosmetic → **visual-gated**. No headless unit test is meaningful for an ImGui palette (the value is "does it look right," which only the running editor shows). Verify in-app: panels/title bars/menus are deep charcoal; buttons/tabs/sliders/checkmarks/selection use the subtle blue accent on hover/active; text is crisp and readable; the node editor canvas matches the panels; nothing is unreadably low-contrast. Exact values are easy to tune at the gate (that's why they're named constants).

## Files
**Modified:**
- `engine/editor/ImGuiLayer.cpp` — the named palette constants + `applyIronDarkTheme(ImGuiStyle&)` helper + the call after `StyleColorsDark()`.
- `engine/editor/NodeGraphPanel.cpp` — small `ed::Style` color nudge in the constructor for canvas/panel harmony.

## Notes for the plan
- Branch M60 off `main` after M59 (PR #91) merges (it does not depend on M59 code, but keeps lineage clean). M59 is merged.
- Tab enum names confirmed against the vendored ImGui 1.92.8 header (`ImGuiCol_Tab`/`TabHovered`/`TabSelected`/`TabSelectedOverline`/`TabDimmed`/`TabDimmedSelected`); the old `TabActive`/`TabUnfocused*` names still exist as aliases but use the new names.
- It's one milestone, essentially one styling function + a small canvas nudge — expect a single implementation task plus the visual gate.
- Keep the constants as the single source of truth; assign from them (no duplicated literals in the assignment block).
