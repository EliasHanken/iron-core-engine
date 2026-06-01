# M42 — CollisionShape + AudioEmitter Components (Design)

**Date:** 2026-06-01
**Status:** Design — awaiting user review before plan
**Depends on:** M37 (`iron::World`), M38 (`iron::Reflection`), M39 (reflection-driven Inspector + SceneIO), M41 (Play/Stop mode + `World` deep-copy + sandbox snapshot/restore), M18 (`iron::PhysicsWorld`), M26 (`iron::AudioEngine`)

## Goal

Complete the editor-authoring loop — "place objects, add collision, set audio" — by adding two authorable components to the reflection-driven component model, surfaced in the Inspector and serialized via SceneIO, and exercised at runtime during M41 Play mode. Authoring a collider lets an object collide / fall during Play; authoring an emitter makes an object emit positional (optionally looping) sound during Play. Stop restores the authored state via M41's existing snapshot/restore.

## Background — the architecture this slots into

The editor authors the on-disk model `iron::SceneEntity` (`engine/scene/SceneFormat.h`): a fixed struct holding `name`, `Transform`, `MeshRef`, `MaterialDef`. The `iron::World` is a parallel runtime mirror the renderer iterates (built from `SceneEntity` at load; re-synced from `scene.entities[]` each Edit frame via the existing mirror block in `games/11-sandbox/main.cpp`). The Inspector (`SceneInspector::draw`) hardcodes `renderComponent(reflection, e.transform/mesh/material)`. Save/load goes through `saveSceneFile`/`loadSceneFile` + the generic `ReflectionIO` helpers.

Unifying the two models (editor reads `World` directly) is the deferred **World migration** milestone. M42 deliberately works *within* the current dual model: new components are added as optional fields on `SceneEntity`, not as a generic `World` component bag. This is the least-surprising fit and keeps M42 scoped.

What is already automatic once a component type is registered with `iron::Reflection`:
- **Serialization** — `ReflectionIO::componentToJson`/`componentFromJson` walk `fieldsOf<T>()`. Supports Bool, Int32/UInt32, UInt8, Float, String, Vec3, Quat, Enum, OptionalEnum.
- **Inspector widgets** — `ReflectionInspector::renderComponent<T>` does the same dispatch.
- **Reset on Stop** — M41 snapshots/restores the whole `SceneFile` + `World` + camera.

The genuinely new work is (a) the two component structs + their reflection sidecars, (b) the Add/Remove-Component editor affordance, (c) the Play-mode runtime hookup (build physics bodies / audio voices from the authored components), and (d) a small looping-voice API on `AudioEngine`.

## Components (data model)

New `engine/world/CollisionShape.h`:
```cpp
enum class ColliderShape { Box, Sphere, Capsule };
enum class ColliderBody  { Static, Dynamic };

struct CollisionShape {
    ColliderShape shape       = ColliderShape::Box;
    ColliderBody  body        = ColliderBody::Static;
    Vec3          halfExtents  = {0.5f, 0.5f, 0.5f};  // Box
    float         radius       = 0.5f;                // Sphere / Capsule
    float         halfHeight   = 0.5f;                // Capsule
    float         mass         = 1.0f;                // Dynamic only
};
```

New `engine/audio/AudioEmitter.h`:
```cpp
struct AudioEmitter {
    std::string wavPath;
    float       gain        = 1.0f;
    bool        loop        = true;
    bool        spatial     = true;   // positional (playLooping/playSoundAt) vs. 2D (local)
    bool        playOnStart = true;   // auto-start on Edit->Play
};
```

Both are POD (standard-layout, default-constructible) per the `iron::Reflection` `static_assert` requirements. Both are added as optional fields on `SceneEntity`:
```cpp
struct SceneEntity {
    std::string                 name;
    Transform                   transform;
    MeshRef                     mesh;
    MaterialDef                 material;
    std::optional<CollisionShape> collision;  // M42
    std::optional<AudioEmitter>   audio;       // M42
};
```
Absent optional = the entity has no collider / emitter. On-disk JSON omits absent optionals (additive, backward-compatible — existing `demo.json` loads unchanged; no migration needed).

## Reflection sidecars

`engine/world/CollisionShape.reflect.cpp` and `engine/audio/AudioEmitter.reflect.cpp`, following the existing `register*(Reflection&)` pattern:
- `registerEnum<ColliderShape>` (Box/Sphere/Capsule), `registerEnum<ColliderBody>` (Static/Dynamic).
- `registerType<CollisionShape>` with fields; `halfExtents`/`radius`/`halfHeight`/`mass` get sensible `min` meta (e.g. `radius {.min = 0.001f}`, `mass {.min = 0.0f}`).
- `registerType<AudioEmitter>` with fields; `gain {.min = 0.0f, .max = 2.0f, .slider = true}`.

The sandbox host calls `registerCollisionShape(reflection)` + `registerAudioEmitter(reflection)` alongside the existing four registrations.

No `ReflectionIO`/`ReflectionInspector` engine changes are required — `bool`, `float`, `string`, `Vec3`, and `enum` are all already in the dispatch. (Verified against `ReflectionInspector.h` and `ReflectionIO.h` field-type lists.)

## SceneIO

`saveSceneFile`/`loadSceneFile` (`engine/scene/SceneIO.cpp`) gain handling for the two optionals: on save, if present, emit `"collision": componentToJson(r, *e.collision)` / `"audio": …`; on load, if the key exists, default-construct + `componentFromJson`. Mirrors how `transform`/`mesh`/`material` are already written, just gated on the optional. Round-trip covered by unit tests.

## Inspector — Add/Remove Component affordance

`SceneInspector::draw` gains:
1. **Render present optionals** — after the existing transform/mesh/material, `if (e.collision) renderComponent(reflection, *e.collision)` (with a `[Remove]` button that resets the optional), same for `e.audio`.
2. **"Add Component ▾" combo** — lists the optional component types this entity does *not* currently have; selecting one attaches a default-constructed instance.

Driven by a small table-driven descriptor list (in `SceneInspector.cpp`), one entry per optional component:
```cpp
struct OptionalComponentDesc {
    const char* label;                       // "CollisionShape"
    bool (*present)(const SceneEntity&);     // e.collision.has_value()
    void (*attach)(SceneEntity&);            // e.collision.emplace()
    void (*remove)(SceneEntity&);            // e.collision.reset()
    bool (*render)(const Reflection&, SceneEntity&); // renderComponent(*e.collision)
};
```
The combo + render loop iterate this table generically (M42 seeds it with two entries: CollisionShape and AudioEmitter). Adding any future optional component = one table entry + one `SceneEntity` field.

**Honest scope note:** a *fully* generic "list any registered component and `world.add<T>` it" combo is not possible while the editor authors the fixed `SceneEntity` struct — attaching to `e.collision` vs `e.audio` is inherently per-field. The table-driven form is generic over the descriptor list (the UI code doesn't grow per component); the zero-code version arrives with the World migration.

`draw` returns `changed` as today; add/remove also count as changes (trigger the existing scene→World re-sync + mark dirty).

## Play-mode runtime hookup (sandbox `main.cpp`)

State added near the M41 Play/Stop state:
- `std::unordered_map<int, BodyId> playBodies_;` (scene-entity index → Jolt body)
- `std::unordered_map<int, VoiceId> playVoices_;` (scene-entity index → audio voice)
- `iron::AudioEngine audio;` constructed + `audio.init()` at startup (M41 deferred this); `audio.setListener(cam.position, cam.forward(), cam.up())` once per frame.
- A `SoundHandle` cache keyed by wav path (avoid reloading every Play).

**`spawnRuntime()`** (called on Edit→Play, replacing M41's debug-cube `spawnDebugBodies`; the M41 magenta debug cube is removed — real authored colliders supersede it):
- For each `scene.entities[i]` with `collision`: create the matching Jolt body from shape + size + `transform` + (dynamic) mass. Static → `createStaticBox/Sphere?`… (Jolt wrapper currently exposes `createStaticBox`; static sphere/capsule may need a thin wrapper addition — see Risks). Store in `playBodies_`.
- For each with `audio` and `playOnStart`: ensure the wav is loaded/cached; start a voice — `playLooping` (spatial+loop), `playSoundAt` (spatial one-shot), or `playSoundLocal` (2D). Store in `playVoices_` (one-shots aren't stored for stop; looping voices are).

**Each Play step** (after `physics.step`):
- For every Dynamic body in `playBodies_`, write `physics.bodyPosition`/`bodyRotation` into that entity's **World** `Transform` component (render reads World), so the mesh moves. The scene-entity-index → World `EntityId` mapping is built at load (entities are created 1:1 in order; the sandbox already tracks `resolved[].entityIndex`).
- For every spatial looping voice whose entity has a Dynamic body, `audio.setVoicePosition(voice, newPos)` so the sound follows the object.

**`despawnRuntime()`** (Play→Edit): `destroyBody` every entry in `playBodies_`; `audio.stop` every entry in `playVoices_`; clear both maps. M41's snapshot restore resets all transforms — no manual transform reset.

## AudioEngine looping-voice API

`engine/audio/AudioEngine.h/.cpp` gains a tracked-voice concept:
```cpp
using VoiceId = std::uint32_t;
constexpr VoiceId kInvalidVoice = 0;

VoiceId playLooping(SoundHandle h, Vec3 worldPos, float gain = 1.0f); // AL_LOOPING source
void    stop(VoiceId v);                                              // halt + free the source
void    setVoicePosition(VoiceId v, Vec3 worldPos);                   // re-position a live voice
```
Implementation: allocate from the existing source pool but mark a looping voice as non-stealable while playing; return a `VoiceId` that maps to the OpenAL source. `stop` halts + returns the source to the pool. No-op + `kInvalidVoice` when the engine failed to init (headless). One-shot `playSoundAt`/`playSoundLocal` paths are unchanged.

## Edit-mode collider visualization

In Edit mode, draw a green wireframe of each collider via the existing `Renderer::drawLineOverlay` (the API M41 already used): box → 12 edges from `transform` + `halfExtents`; sphere → 3 great-circle rings at `radius`; capsule → two hemisphere rings + side lines. Draw for all entities that have a `collision` component (Unity/Unreal convention). Cheap; if it clutters, a single bool toggle (default on) limits it to the selected entity. No collider wireframe in Play mode (the geometry itself shows the result).

## Testing

- `tests/test_collision_shape.cpp` — defaults; `ReflectionIO` round-trip (to JSON and back preserves every field + enum); optional present/absent serialization.
- `tests/test_audio_emitter.cpp` — same shape (defaults + round-trip + bool/string fields).
- `tests/test_scene_io.cpp` — extend with an entity carrying both optionals: save → load → fields + presence preserved; an entity without them stays absent.
- AudioEngine looping is hardware-dependent → no automated audio test (the no-op-on-headless contract keeps it safe); validated in the visual gate.
- Target: 52 → ~55 CTest cases.

**Visual gate:** author a Box/Dynamic collider on a cube; add a Box/Static collider on the floor plane → Play → cube falls and lands on the floor (mesh actually moves, not a debug wireframe). Add a looping spatial emitter to the cube → hear it, panning as the camera moves, following the cube as it falls. Stop → cube returns to authored pose, sound stops. Save → reload → colliders + emitter persist.

## Non-goals (deferred)

- Kinematic bodies; physics materials (restitution / friction); per-emitter rolloff/falloff tuning.
- Trigger / sensor volumes + contact events surfaced to gameplay/script (a scripting-track concern).
- Generic `world.add<T>` Add-Component combo (needs World migration).
- Collider auto-fit beyond the default — v1 defaults `halfExtents` to the unit box; the Inspector lets the user size it. (Auto-fit from `localBounds` is a cheap follow-up; see Risks.)
- Multi-emitter per entity; emitter start/stop driven by gameplay events rather than Play start.

## Risks / open implementation questions (for the plan)

1. **Jolt wrapper coverage.** `PhysicsWorld` exposes `createStaticBox`, `createDynamicBox/Sphere/Capsule`. Static *sphere/capsule* and dynamic boxes-from-rotation may need thin wrapper additions. The plan should confirm the exact create-call matrix and add minimal wrappers if a shape×bodytype cell is missing. Rotation: `createDynamicBox` takes pos + halfExtents only — authored rotation may not reach Jolt at create time (M18 noted "wrapper doesn't yet accept rotation at creation"). Box colliders on rotated entities are a flagged limitation unless we extend the wrapper.
2. **Auto-fit from mesh bounds.** The sandbox already computes per-entity `localBounds` (model-space AABB). Defaulting a new Box collider's `halfExtents` to `localBounds` half-size on attach is a nice UX win and cheap — decide in the plan whether to include it in v1 or defer.
3. **Scene-index ↔ World-EntityId map.** Confirm/construct the 1:1 map at load for dynamic write-back (entities are created in order; `resolved[].entityIndex` exists).
4. **Voice pool exhaustion.** Many looping emitters could exhaust the 32-source pool. v1 accepts this (log a warn-once); a priority/virtualization scheme is future work.
