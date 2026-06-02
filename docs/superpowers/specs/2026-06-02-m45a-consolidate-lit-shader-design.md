# M45a — Consolidate the Lit Shader (design)

**Date:** 2026-06-02
**Status:** approved (brainstorm), pending implementation plan
**Track:** Render-polish detour → M45 PBR, split into **M45a (this spec, consolidation)** then **M45b (Cook-Torrance PBR swap)**.

---

## Why this milestone

The Vulkan "standard lit" shader (shadow PCF + normal map + Blinn-Phong spec + point lights + planar/cubemap reflection + fog + emissive) is **copy-pasted across 6 games** as inline GLSL strings (net-shooter loads it from files). All copies consume the same shared `LitUbo` (928 B) and the same 7-binding descriptor-set contract; only the *source* is duplicated. M44 already demonstrated the cost (4 `aces()` copies kept in lockstep). M45b (PBR) and the later IBL/SSAO milestones each rewrite this shader — doing that across 6 copies is error-prone.

M45a makes the standard lit shader **engine-owned and shared**, with a **pixel-identical** result (still Blinn-Phong), so M45b can swap exactly one shader to Cook-Torrance. This is a pure de-risk refactor — the same split that worked for M43a/M43b.

## Goals
- One engine-owned source of truth for the standard lit shader (vert + skinned-vert + frag).
- A renderer factory games call instead of pasting GLSL.
- All 6 standard-lit Vulkan games migrated; each renders **identical** to before.
- No behavior change, no `LitUbo`/descriptor-layout change.

## Non-goals (deferred)
- Any lighting/BRDF change — that is M45b (Cook-Torrance).
- `Material`/`LitUbo`/descriptor-layout changes — M45b.
- Migrating non-standard-lit games (see Scope below).
- A general shader hot-reload / material system.

---

## Scope

**In scope — migrate these 6 Vulkan games** (they already use the full standard lit shader with `materialParams`):
- `games/01-spinning-cube` (lit)
- `games/03-showcase` (lit)
- `games/07-net-shooter` (lit + skinned; currently file-based)
- `games/09-physics-playground` (lit)
- `games/10-gltf-viewer` (lit + skinned)
- `games/11-sandbox` (lit) — the editor host

**Out of scope (left untouched):**
- `games/05-net-cubes`, `games/06-net-tag` — use a *simpler* shader (no `materialParams`/normal/reflection bindings); not the standard lit shader. Not part of consolidation.
- OpenGL games (`games/02-strandbound`, and any GL net demos) — frozen backend, separate GLSL 330 path.

## Architecture / components

### 1. Engine-owned shader sources
**New `engine/render/StandardLitShader.h`** exposing:
- `const char* standardLitVertSource();` — canonical lit vertex shader (Vulkan GLSL 450): transforms position, emits world pos / normal / tangent / uv / light-space pos.
- `const char* standardSkinnedLitVertSource();` — skinned variant (SkinnedVertex input: joints/weights + bone matrices), emitting the same outputs.
- `const char* standardLitFragSource();` — the canonical fragment shader (shadow PCF, normal map, Blinn-Phong spec, point lights, planar+cubemap reflection, fog, emissive). Basis = net-shooter's current `lit.frag.glsl`.

These are the single source of truth. Header-only `inline const char*` (or a `.cpp`) — match how the engine ships other embedded GLSL (e.g. `VkPostProcess` inline strings).

### 2. Renderer factory
On the `Renderer` interface (Vulkan-backed; OpenGL games don't call it):
- `ShaderHandle createStandardLitShader();` → `createShader(standardLitVertSource(), standardLitFragSource())`.
- `ShaderHandle createStandardSkinnedLitShader();` → `createSkinnedShader(standardSkinnedLitVertSource(), standardLitFragSource())`.

Thin wrappers over the existing `createShader`/`createSkinnedShader` + the embedded sources. No new compile path.

### 3. Game migration
For each of the 6 games: replace the inline `kVertexShader`/`kFragmentShader` (and skinned variants) + their `createShader(...)` calls with the factory calls; delete the now-dead inline GLSL strings. For net-shooter: also delete `assets/shaders/lit.vert.glsl` / `lit.frag.glsl` / `lit-skinned.vert.glsl`, remove their file-load code, and remove the asset-copy of those files from its build if applicable.

## Reconciliation policy (the only behavioral subtlety)
- **Alpha output:** canonical = `outColor = vec4(finalColor, 1.0)`. Scene-pass meshes are opaque (no blending on the scene target), so this matches every standard-lit game. (The `texel.a` form only appears in the out-of-scope simpler games.)
- **Superset safety:** the canonical frag is a superset; simpler games (e.g. spinning-cube: no point lights, no reflection) get identical output because the point-light loop no-ops at `count=0` and the reflection branch no-ops at `reflectivity=0`/`useReflectionPlane=0` — and `recordSceneDraw` already fills `LitUbo` uniformly for all games.
- **Vertex shaders** are unified into the one canonical vert (and one skinned vert). Any trivial per-game vert difference is reconciled into the shared source during migration; the visual gate catches regressions.

## Accepted behavior change (call-out for spec review)
**net-shooter loses its file-based shader hot-reload.** Today net-shooter reloads `lit.*.glsl` from disk on a keypress (a dev convenience using `reloadShader`). Once its shader is engine-embedded, there are no files to edit at runtime, so that hot-reload is removed. This is an accepted trade for consolidation (the shader is now engine-owned; runtime GLSL editing isn't a project goal). If you want to keep a hot-reload path, say so at spec review and we'll scope it separately.

## Data flow
Unchanged. Same `LitUbo`, same descriptor-set layout, same `recordSceneDraw`, same passes. Only the shader *source* moves from per-game to engine. Each game still owns its shader *handle* (from the factory) and uses it in its draw calls exactly as before.

## Testing
- **Unit (`tests/test_standard_lit_shader.cpp`):** compile `standardLitVertSource()`, `standardSkinnedLitVertSource()`, and `standardLitFragSource()` through the engine's glslang path (mirror `test_glsl_to_spirv`) and assert each produces non-empty SPIR-V. Guards the embedded sources against syntax rot. CTest +1.
- **Visual gate (user):** launch each of the 6 migrated Vulkan games and confirm each renders identical to before:
  - sandbox, net-shooter, showcase, physics-playground, gltf-viewer (incl. a skinned/animated model), spinning-cube.

## Risks
- **Subtle per-game shader tweak** not spotted during extraction → caught by the per-game visual gate; reconcile into the shared source.
- **Skinned variant divergence** (gltf-viewer vs net-shooter skinned vert) → unify carefully; gate the animated model.
- **net-shooter asset/build breakage** when removing its shader files (asset-copy step) → verify net-shooter still builds + runs.
- **Out-of-scope games untouched** — do NOT migrate net-cubes/net-tag or any OpenGL game.

## Open question for spec review
- Keep net-shooter's shader hot-reload (scoped separately), or accept its removal? (Lean: accept removal — runtime GLSL editing isn't a project goal and the shader is now engine-owned.)
