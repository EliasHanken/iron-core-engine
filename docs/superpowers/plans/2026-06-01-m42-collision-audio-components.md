# M42 — CollisionShape + AudioEmitter Components Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two authorable components — `CollisionShape` (Box/Sphere/Capsule × Static/Dynamic + mass) and `AudioEmitter` (wavPath/gain/loop/spatial/playOnStart) — to the iron-core editor, surfaced in the Inspector + serialized via SceneIO, and exercised in M41 Play mode (build Jolt bodies + audio voices on Edit→Play; dynamic bodies move their mesh; Stop restores via M41 snapshot).

**Architecture:** Both components are POD structs added as `std::optional` fields on `iron::SceneEntity`. Serialization + Inspector widgets are automatic via the M38/M39 reflection layer (no engine changes to ReflectionIO/ReflectionInspector — Bool/Float/String/Vec3/Enum all already supported). New engine surface: an `AudioEngine` looping-voice API (`VoiceId` + `playLooping`/`stop`/`setVoicePosition`) and `PhysicsWorld` static-sphere/capsule + rotation-at-create. The sandbox owns the Play-mode runtime: it iterates `scene.entities`, builds bodies/voices, writes dynamic-body transforms back into `scene.entities[i].transform` (the existing unconditional scene→World mirror then propagates them to the renderer), and tears everything down on Stop.

**Tech Stack:** C++23 (MSVC `/std:c++latest`), CMake (no presets — build dir `build-vk`), Vulkan-only. `iron::Reflection` (M38), `ReflectionIO`/`ReflectionInspector` (M39), `iron::PhysicsWorld` (Jolt, M18), `iron::AudioEngine` (OpenAL Soft + dr_wav, M26), `iron::World` + `EditorState` (M37/M41), Dear ImGui (in `ironcore_editor`). Reference spec: `docs/superpowers/specs/2026-06-01-m42-collision-audio-components-design.md`.

**Branch:** already on `feat/m42-collision-audio` (spec commit `12d4c02` is on `main`; this branch is cut from it). Every task commits to this branch.

**Build & test commands (used throughout):**
```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target ironcore_editor
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
(A benign "LF will be replaced by CRLF" git warning is expected on Windows. Pre-existing ImGui/GLFW `LNK4217` linker warnings are benign. `test_renderer_factory` skips itself when `IRON_CI=1`.)

**Baseline:** 52 CTest cases as of M41 (PR #62, `cf94390`).

---

## Current state of the codebase (read before starting)

- **`iron::SceneEntity`** (`engine/scene/SceneFormat.h`) — fixed struct: `name`, `Transform transform`, `MeshRef mesh`, `MaterialDef material`. This is the on-disk + editor-authoring model. The `iron::World` is a runtime mirror the renderer iterates.
- **Reflection sidecars** — each component has a `register<T>(Reflection&)` free function in a `*.reflect.cpp` (e.g. `engine/world/Transform.reflect.cpp`), declared in `engine/reflection/RegisterCoreTypes.h`, listed in `engine/CMakeLists.txt`'s `ironcore` sources, and called by the sandbox at startup.
- **`ReflectionIO`** (`engine/scene/ReflectionIO.{h,cpp}`) + **`ReflectionInspector`** (`engine/editor/ReflectionInspector.{h,cpp}`) — generic, driven by `fieldsOf<T>()`. `TypeId` (`engine/reflection/TypeId.h`) supports Bool, Int32, UInt32, UInt8, Float, String, Vec3, Quat, Enum, OptionalEnum. **No changes needed** — `CollisionShape` (Vec3/float/enum) and `AudioEmitter` (string/float/bool) are fully covered.
- **`SceneIO`** (`engine/scene/SceneIO.cpp`) — `entityToJson`/`entityFromJson` hardcode `transform`/`mesh`/`material` keys via `componentToJson`/`componentFromJson`. We add `collision`/`audio` keys gated on the optionals.
- **`SceneInspector::draw`** (`engine/editor/SceneInspector.cpp`) — hardcodes `renderComponent(reflection, e.transform/mesh/material)`. We add render-if-present + an Add/Remove-Component combo.
- **`PhysicsWorld`** (`engine/physics/PhysicsWorld.{h,cpp}`) — `createBodyImpl(impl, shape, pos, motion, layer, mass)` is the generic internal creator (hardcodes `JPH::Quat::sIdentity()`). Public creators: `createStaticBox`, `createDynamicBox/Sphere/Capsule`. **Missing:** static sphere/capsule + rotation-at-create.
- **`AudioEngine`** (`engine/audio/AudioEngine.{h,cpp}`) — 32-source pool, scan-for-idle + round-robin voice-steal. One-shot only (`playSoundAt`/`playSoundLocal`). No looping, no stoppable handles. No-op + safe when `init()` failed (headless).
- **Sandbox** (`games/11-sandbox/main.cpp`) — relevant landmarks:
  - `std::vector<iron::EntityId> sceneIndexToEntity;` (line ~326) maps scene index → World `EntityId` (parallel to `scene.entities`).
  - Reflection registration block (lines ~294–298).
  - `AudioEngine` is **not** constructed (M41 deferred it).
  - M41 Play/Stop state + `spawnDebugBodies`/`despawnDebugBodies`/`togglePlayMode` (lines ~450–504). **These get replaced.**
  - `physics.step(t.deltaSeconds)` in setUpdate gated on `editor.isPlaying()` (lines ~627–630).
  - Unconditional scene→World mirror in setRender (lines ~928–939): `*t = se.transform`. **This is why write-back targets `scene.entities`, not World.**
  - M41 magenta debug-cube wireframe block in setRender (lines ~1012–1048). **Gets replaced** by collider wireframes.

---

## File structure

**New (engine):**
- `engine/world/CollisionShape.h` — `ColliderShape`/`ColliderBody` enums + `CollisionShape` POD
- `engine/world/CollisionShape.reflect.cpp` — `registerCollisionShape(Reflection&)`
- `engine/audio/AudioEmitter.h` — `AudioEmitter` POD
- `engine/audio/AudioEmitter.reflect.cpp` — `registerAudioEmitter(Reflection&)`

**New (tests):**
- `tests/test_collision_shape.cpp`
- `tests/test_audio_emitter.cpp`

**Modified (engine):**
- `engine/reflection/RegisterCoreTypes.h` — declare the two new register fns
- `engine/CMakeLists.txt` — add the two `.reflect.cpp` to `ironcore`
- `engine/scene/SceneFormat.h` — two `std::optional` fields on `SceneEntity`
- `engine/scene/SceneIO.cpp` — serialize/deserialize the two optionals
- `engine/audio/AudioEngine.{h,cpp}` — `VoiceId` looping API
- `engine/physics/PhysicsWorld.{h,cpp}` — static sphere/capsule + rotation-at-create
- `engine/editor/SceneInspector.cpp` — render-if-present + Add/Remove combo
- `tests/CMakeLists.txt` — register the two new tests
- `tests/test_scene_io.cpp`, `tests/test_physics_world.cpp`, `tests/test_audio_engine.cpp` — append subtests

**Modified (games):**
- `games/11-sandbox/main.cpp` — register types, construct `AudioEngine`, replace M41 debug bodies with real `spawnRuntime`/`despawnRuntime`, dynamic write-back, listener + voice tracking, collider wireframes

**Untouched on purpose:** Renderer (Vulkan/GL), `ReflectionIO`/`ReflectionInspector` engine code, `World`, shipping games (`net-shooter`, `02-strandbound`, …), all other reflection sidecars.

---

## Phases

- **A — Pure-logic engine additions (TDD):** A1 CollisionShape, A2 AudioEmitter, A3 AudioEngine looping API, A4 PhysicsWorld static-sphere/capsule + rotation. 4 tasks.
- **B — Data model + Inspector:** B1 SceneEntity optionals + SceneIO, B2 Inspector Add/Remove combo. 2 tasks.
- **C — Sandbox runtime:** C1 build bodies/voices + write-back + listener, C2 collider wireframes. 2 tasks.
- **D — Visual gate + PR + merge + memory.** 1 task.

Total: 9 tasks.

---

## Phase A — Pure-logic engine additions (TDD)

### Task A1: `CollisionShape` component + reflection + test

**Files:**
- Create: `engine/world/CollisionShape.h`
- Create: `engine/world/CollisionShape.reflect.cpp`
- Create: `tests/test_collision_shape.cpp`
- Modify: `engine/reflection/RegisterCoreTypes.h`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_collision_shape.cpp`:
```cpp
#include "reflection/Reflection.h"
#include "reflection/RegisterCoreTypes.h"
#include "scene/ReflectionIO.h"
#include "world/CollisionShape.h"
#include "test_framework.h"

int main() {
    // --- Defaults ---
    {
        iron::CollisionShape c;
        CHECK(c.shape == iron::ColliderShape::Box);
        CHECK(c.body  == iron::ColliderBody::Static);
        CHECK_NEAR(c.halfExtents.x, 0.5f);
        CHECK_NEAR(c.radius, 0.5f);
        CHECK_NEAR(c.halfHeight, 0.5f);
        CHECK_NEAR(c.mass, 1.0f);
    }

    // --- ReflectionIO round-trip preserves every field + both enums ---
    {
        iron::Reflection r;
        iron::registerCollisionShape(r);

        iron::CollisionShape src;
        src.shape       = iron::ColliderShape::Capsule;
        src.body        = iron::ColliderBody::Dynamic;
        src.halfExtents = {1.0f, 2.0f, 3.0f};
        src.radius      = 0.75f;
        src.halfHeight  = 1.25f;
        src.mass        = 9.0f;

        const nlohmann::json j = iron::componentToJson(r, src);
        iron::CollisionShape dst;
        iron::componentFromJson(r, dst, j);

        CHECK(dst.shape == iron::ColliderShape::Capsule);
        CHECK(dst.body  == iron::ColliderBody::Dynamic);
        CHECK_NEAR(dst.halfExtents.x, 1.0f);
        CHECK_NEAR(dst.halfExtents.y, 2.0f);
        CHECK_NEAR(dst.halfExtents.z, 3.0f);
        CHECK_NEAR(dst.radius, 0.75f);
        CHECK_NEAR(dst.halfHeight, 1.25f);
        CHECK_NEAR(dst.mass, 9.0f);
    }

    return iron_test_result();
}
```

Register it in `tests/CMakeLists.txt` — append after the `iron_add_test(test_reflection_io …)` line (`iron_add_test` links `ironcore`, which now carries the new sidecar):
```cmake
iron_add_test(test_collision_shape test_collision_shape.cpp)
```

- [ ] **Step 2: Run the failing test**
```bash
cmake --build build-vk --config Debug --target test_collision_shape
```
Expected: compile error — `world/CollisionShape.h` does not exist.

- [ ] **Step 3: Create `engine/world/CollisionShape.h`**
```cpp
#pragma once

#include "math/Vec.h"

namespace iron {

// What primitive the collider uses. Matches PhysicsWorld's shape set.
enum class ColliderShape { Box, Sphere, Capsule };

// Static = immovable collider (floors, walls). Dynamic = rigid body that
// falls / collides; its mesh transform is driven by physics during Play.
enum class ColliderBody { Static, Dynamic };

// Authorable collider on an entity. Built into a Jolt body on Edit->Play
// (host/sandbox owns the body lifetime); reset by M41 snapshot on Stop.
struct CollisionShape {
    ColliderShape shape       = ColliderShape::Box;
    ColliderBody  body        = ColliderBody::Static;
    Vec3          halfExtents = {0.5f, 0.5f, 0.5f};  // Box
    float         radius      = 0.5f;                // Sphere / Capsule
    float         halfHeight  = 0.5f;                // Capsule (cylinder half-height)
    float         mass        = 1.0f;                // Dynamic only
};

}  // namespace iron
```

- [ ] **Step 4: Create `engine/world/CollisionShape.reflect.cpp`**
```cpp
#include "reflection/Reflection.h"
#include "world/CollisionShape.h"

namespace iron {

void registerCollisionShape(Reflection& r) {
    r.registerEnum<ColliderShape>("ColliderShape")
        .value("box",     ColliderShape::Box)
        .value("sphere",  ColliderShape::Sphere)
        .value("capsule", ColliderShape::Capsule);
    r.registerEnum<ColliderBody>("ColliderBody")
        .value("static",  ColliderBody::Static)
        .value("dynamic", ColliderBody::Dynamic);
    r.registerType<CollisionShape>("CollisionShape")
        .field("shape",       &CollisionShape::shape)
        .field("body",        &CollisionShape::body)
        .field("halfExtents", &CollisionShape::halfExtents, {.min = 0.001f})
        .field("radius",      &CollisionShape::radius,      {.min = 0.001f})
        .field("halfHeight",  &CollisionShape::halfHeight,  {.min = 0.001f})
        .field("mass",        &CollisionShape::mass,        {.min = 0.0f});
}

}  // namespace iron
```

- [ ] **Step 5: Declare the register fn in `engine/reflection/RegisterCoreTypes.h`**

Add below `registerRenderHandles`:
```cpp
void registerCollisionShape(Reflection& r);
```

- [ ] **Step 6: Add the sidecar to `engine/CMakeLists.txt`**

In the `add_library(ironcore STATIC …)` source list, after `world/Transform.reflect.cpp`:
```cmake
  world/CollisionShape.reflect.cpp
```

- [ ] **Step 7: Build + run**
```bash
cmake --build build-vk --config Debug --target test_collision_shape
cd build-vk && ctest -C Debug -R test_collision_shape --output-on-failure -V
```
Expected: `OK - all checks passed`; 1/1 passed.

- [ ] **Step 8: Commit**
```bash
git add engine/world/CollisionShape.h engine/world/CollisionShape.reflect.cpp engine/reflection/RegisterCoreTypes.h engine/CMakeLists.txt tests/test_collision_shape.cpp tests/CMakeLists.txt
git commit -m "M42: CollisionShape component + reflection + test"
```

---

### Task A2: `AudioEmitter` component + reflection + test

**Files:**
- Create: `engine/audio/AudioEmitter.h`
- Create: `engine/audio/AudioEmitter.reflect.cpp`
- Create: `tests/test_audio_emitter.cpp`
- Modify: `engine/reflection/RegisterCoreTypes.h`
- Modify: `engine/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_audio_emitter.cpp`:
```cpp
#include "audio/AudioEmitter.h"
#include "reflection/Reflection.h"
#include "reflection/RegisterCoreTypes.h"
#include "scene/ReflectionIO.h"
#include "test_framework.h"

int main() {
    // --- Defaults ---
    {
        iron::AudioEmitter a;
        CHECK(a.wavPath.empty());
        CHECK_NEAR(a.gain, 1.0f);
        CHECK(a.loop == true);
        CHECK(a.spatial == true);
        CHECK(a.playOnStart == true);
    }

    // --- ReflectionIO round-trip preserves every field (incl. bools) ---
    {
        iron::Reflection r;
        iron::registerAudioEmitter(r);

        iron::AudioEmitter src;
        src.wavPath     = "assets/cc0/sfx/hum.wav";
        src.gain        = 0.4f;
        src.loop        = false;
        src.spatial     = false;
        src.playOnStart = false;

        const nlohmann::json j = iron::componentToJson(r, src);
        iron::AudioEmitter dst;
        iron::componentFromJson(r, dst, j);

        CHECK(dst.wavPath == "assets/cc0/sfx/hum.wav");
        CHECK_NEAR(dst.gain, 0.4f);
        CHECK(dst.loop == false);
        CHECK(dst.spatial == false);
        CHECK(dst.playOnStart == false);
    }

    return iron_test_result();
}
```

Register in `tests/CMakeLists.txt`:
```cmake
iron_add_test(test_audio_emitter test_audio_emitter.cpp)
```

- [ ] **Step 2: Run the failing test**
```bash
cmake --build build-vk --config Debug --target test_audio_emitter
```
Expected: compile error — `audio/AudioEmitter.h` does not exist.

- [ ] **Step 3: Create `engine/audio/AudioEmitter.h`**
```cpp
#pragma once

#include <string>

namespace iron {

// Authorable sound source on an entity. On Edit->Play the host loads the WAV
// and starts a voice at the entity (looping/positional per the flags); the
// voice is stopped on Stop. One-shot event sounds (gunshots etc.) belong in
// gameplay code, not here — an emitter is for persistent/ambient sound.
struct AudioEmitter {
    std::string wavPath;
    float       gain        = 1.0f;
    bool        loop        = true;   // looping ambience vs. one-shot on start
    bool        spatial     = true;   // positional (3D) vs. 2D (centered)
    bool        playOnStart = true;   // auto-start on Edit->Play
};

}  // namespace iron
```

- [ ] **Step 4: Create `engine/audio/AudioEmitter.reflect.cpp`**
```cpp
#include "audio/AudioEmitter.h"
#include "reflection/Reflection.h"

namespace iron {

void registerAudioEmitter(Reflection& r) {
    r.registerType<AudioEmitter>("AudioEmitter")
        .field("wavPath",     &AudioEmitter::wavPath)
        .field("gain",        &AudioEmitter::gain, {.min = 0.0f, .max = 2.0f, .slider = true})
        .field("loop",        &AudioEmitter::loop)
        .field("spatial",     &AudioEmitter::spatial)
        .field("playOnStart", &AudioEmitter::playOnStart);
}

}  // namespace iron
```

- [ ] **Step 5: Declare in `engine/reflection/RegisterCoreTypes.h`**

Below `registerCollisionShape`:
```cpp
void registerAudioEmitter(Reflection& r);
```

- [ ] **Step 6: Add the sidecar to `engine/CMakeLists.txt`**

After `world/CollisionShape.reflect.cpp`:
```cmake
  audio/AudioEmitter.reflect.cpp
```

- [ ] **Step 7: Build + run**
```bash
cmake --build build-vk --config Debug --target test_audio_emitter
cd build-vk && ctest -C Debug -R test_audio_emitter --output-on-failure -V
```
Expected: `OK - all checks passed`; 1/1 passed.

- [ ] **Step 8: Commit**
```bash
git add engine/audio/AudioEmitter.h engine/audio/AudioEmitter.reflect.cpp engine/reflection/RegisterCoreTypes.h engine/CMakeLists.txt tests/test_audio_emitter.cpp tests/CMakeLists.txt
git commit -m "M42: AudioEmitter component + reflection + test"
```

---

### Task A3: `AudioEngine` looping-voice API (`VoiceId`)

**Files:**
- Modify: `engine/audio/AudioEngine.h`
- Modify: `engine/audio/AudioEngine.cpp`
- Modify: `tests/test_audio_engine.cpp`

A looping voice must be stoppable, so we return a `VoiceId` mapping to a reserved source. Reserved sources are excluded from the one-shot idle-scan + steal so a loop is never silently stolen.

- [ ] **Step 1: Write the failing test (no-op safety on an uninitialized engine)**

`tests/test_audio_engine.cpp` runs headless in CI (no device), so `init()` typically fails and the engine is a no-op. Add subtests that exercise the new API's safe-no-op contract. Append these calls inside `main()` (before the final `return`), matching the file's existing `CHECK`/`iron_test_result` style:
```cpp
    // --- M42: looping-voice API is safe on an uninitialized engine ---
    {
        iron::AudioEngine eng;   // not init()'d (or init may fail headless)
        const iron::VoiceId v = eng.playLooping(iron::kInvalidSound, iron::Vec3{}, 1.0f);
        CHECK(v == iron::kInvalidVoice);
        eng.setVoicePosition(v, iron::Vec3{1.0f, 2.0f, 3.0f});  // no crash
        eng.stop(v);                                            // no crash
    }
```
(If `test_audio_engine.cpp` uses a different assert macro, match it. Include `"audio/AudioEngine.h"` is already present.)

- [ ] **Step 2: Run to verify it fails**
```bash
cmake --build build-vk --config Debug --target test_audio_engine
```
Expected: compile error — `iron::VoiceId` / `kInvalidVoice` / `playLooping` undeclared.

- [ ] **Step 3: Declare the API in `engine/audio/AudioEngine.h`**

After the `SoundHandle` typedef block near the top (after `constexpr SoundHandle kInvalidSound = 0;`), add:
```cpp
using VoiceId = std::uint32_t;
constexpr VoiceId kInvalidVoice = 0;
```

In the `public:` section, after `playSoundLocal(...)`, add:
```cpp
    // Start a looping, positional voice and return a handle so it can be
    // stopped + repositioned later. Used by authored AudioEmitters (M42).
    // The source is reserved (excluded from one-shot voice-stealing) until
    // stop(). Returns kInvalidVoice if the engine failed to init, h is
    // invalid, or no free source is available.
    VoiceId playLooping(SoundHandle h, Vec3 worldPos, float gain = 1.0f);

    // Stop a voice started by playLooping and release its source. No-op on
    // kInvalidVoice or an unknown handle.
    void stop(VoiceId v);

    // Reposition a live voice (e.g. an emitter on a physics-moving entity).
    // No-op on kInvalidVoice or an unknown handle.
    void setVoicePosition(VoiceId v, Vec3 worldPos);
```

- [ ] **Step 4: Implement in `engine/audio/AudioEngine.cpp`**

Add to `struct AudioEngine::Impl` (after `nextHandle`):
```cpp
    // Looping voices: a reserved-source set + VoiceId -> pool-slot map.
    std::array<bool, kSourcePoolSize>      reserved{};
    std::unordered_map<VoiceId, std::size_t> voiceSlot;
    VoiceId                                  nextVoice = 1;
```

In `playSoundAt` AND `playSoundLocal`, the idle-scan loop must skip reserved sources. Change the loop body in BOTH functions from:
```cpp
        const ALuint s = impl_->sources[i];
        ALint state = 0;
        alGetSourcei(s, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) {
            chosen = s;
            break;
        }
```
to:
```cpp
        if (impl_->reserved[i]) continue;            // M42: don't grab a loop voice
        const ALuint s = impl_->sources[i];
        ALint state = 0;
        alGetSourcei(s, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) {
            chosen = s;
            break;
        }
```
And the round-robin steal in BOTH functions must skip reserved slots. Change:
```cpp
        chosen = impl_->sources[impl_->nextSource];
        impl_->nextSource = (impl_->nextSource + 1) % impl_->sources.size();
        alSourceStop(chosen);
```
to:
```cpp
        // Advance past reserved (looping) slots; bail if every slot is reserved.
        std::size_t scanned = 0;
        while (impl_->reserved[impl_->nextSource] && scanned < impl_->sources.size()) {
            impl_->nextSource = (impl_->nextSource + 1) % impl_->sources.size();
            ++scanned;
        }
        if (scanned >= impl_->sources.size()) return;   // all reserved; drop this one-shot
        chosen = impl_->sources[impl_->nextSource];
        impl_->nextSource = (impl_->nextSource + 1) % impl_->sources.size();
        alSourceStop(chosen);
```

Add the three new methods near `playSoundLocal` (before `setListener`):
```cpp
VoiceId AudioEngine::playLooping(SoundHandle h, Vec3 worldPos, float gain) {
    if (!impl_->initialized || h == kInvalidSound) return kInvalidVoice;
    auto it = impl_->buffers.find(h);
    if (it == impl_->buffers.end()) return kInvalidVoice;

    // Find a free, non-reserved, non-playing slot.
    std::size_t slot = impl_->sources.size();
    for (std::size_t i = 0; i < impl_->sources.size(); ++i) {
        if (impl_->reserved[i]) continue;
        ALint state = 0;
        alGetSourcei(impl_->sources[i], AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) { slot = i; break; }
    }
    if (slot == impl_->sources.size()) {
        Log::warn("AudioEngine: no free source for looping voice");
        return kInvalidVoice;
    }

    const ALuint src = impl_->sources[slot];
    alSourcei (src, AL_BUFFER,          static_cast<ALint>(it->second.buffer));
    alSource3f(src, AL_POSITION,        worldPos.x, worldPos.y, worldPos.z);
    alSource3f(src, AL_VELOCITY,        0.0f, 0.0f, 0.0f);
    alSourcef (src, AL_GAIN,            gain);
    alSourcei (src, AL_SOURCE_RELATIVE, AL_FALSE);
    alSourcei (src, AL_LOOPING,         AL_TRUE);
    alSourcePlay(src);

    impl_->reserved[slot] = true;
    const VoiceId v = impl_->nextVoice++;
    impl_->voiceSlot[v] = slot;
    return v;
}

void AudioEngine::stop(VoiceId v) {
    if (!impl_->initialized || v == kInvalidVoice) return;
    auto it = impl_->voiceSlot.find(v);
    if (it == impl_->voiceSlot.end()) return;
    const std::size_t slot = it->second;
    const ALuint src = impl_->sources[slot];
    alSourceStop(src);
    alSourcei(src, AL_LOOPING, AL_FALSE);
    alSourcei(src, AL_BUFFER,  0);
    impl_->reserved[slot] = false;
    impl_->voiceSlot.erase(it);
}

void AudioEngine::setVoicePosition(VoiceId v, Vec3 worldPos) {
    if (!impl_->initialized || v == kInvalidVoice) return;
    auto it = impl_->voiceSlot.find(v);
    if (it == impl_->voiceSlot.end()) return;
    alSource3f(impl_->sources[it->second], AL_POSITION, worldPos.x, worldPos.y, worldPos.z);
}
```
(`shutdown()` already stops + deletes every source, so no extra teardown is needed; `voiceSlot`/`reserved` are POD members freed with the Impl.)

- [ ] **Step 5: Build + run all tests**
```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target test_audio_engine
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build; `test_audio_engine` passes; full suite green (54/54 now — A1+A2 added two).

- [ ] **Step 6: Commit**
```bash
git add engine/audio/AudioEngine.h engine/audio/AudioEngine.cpp tests/test_audio_engine.cpp
git commit -m "M42: AudioEngine looping-voice API (VoiceId / playLooping / stop / setVoicePosition)"
```

---

### Task A4: `PhysicsWorld` static sphere/capsule + rotation-at-create

**Files:**
- Modify: `engine/physics/PhysicsWorld.h`
- Modify: `engine/physics/PhysicsWorld.cpp`
- Modify: `tests/test_physics_world.cpp`

Authored colliders can be rotated and can be static spheres/capsules. We thread a rotation through `createBodyImpl` and add the two missing static creators. Rotation is a **trailing default arg** on the existing creators, so current callers (Ragdoll, CharacterController, sandbox M41) keep compiling unchanged.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_physics_world.cpp` — add a test function above `main()` and call it from `main()` before the final return (match the file's existing structure; it uses `CHECK`/`CHECK_NEAR`):
```cpp
static void test_static_sphere_and_capsule_and_rotation() {
    iron::PhysicsWorld w;
    CHECK(w.init());

    // Static sphere + static capsule create live (non-zero) bodies.
    const iron::BodyId s = w.createStaticSphere(iron::Vec3{0, 0, 0}, 1.0f);
    const iron::BodyId c = w.createStaticCapsule(iron::Vec3{3, 0, 0}, 0.5f, 0.3f);
    CHECK(s.isValid());
    CHECK(c.isValid());

    // A dynamic box created with a 90deg-about-Y rotation reports ~that rotation.
    const float h = 0.70710678f;  // sin/cos(45deg) -> quat for 90deg about Y
    const iron::Quat rot{0.0f, h, 0.0f, h};
    const iron::BodyId b = w.createDynamicBox(iron::Vec3{0, 10, 0},
                                              iron::Vec3{0.5f, 0.5f, 0.5f}, 1.0f, rot);
    CHECK(b.isValid());
    const iron::Quat got = w.bodyRotation(b);
    // Compare via |dot| ~ 1 (quaternion double-cover safe).
    const float dot = got.x*rot.x + got.y*rot.y + got.z*rot.z + got.w*rot.w;
    CHECK_NEAR(std::abs(dot), 1.0f);

    w.shutdown();
}
```
Add `#include <cmath>` at the top of the file if not already present, and add `test_static_sphere_and_capsule_and_rotation();` to `main()`.

- [ ] **Step 2: Run to verify it fails**
```bash
cmake --build build-vk --config Debug --target test_physics_world
```
Expected: compile error — `createStaticSphere` / `createStaticCapsule` undeclared and `createDynamicBox` has no 4-arg form.

- [ ] **Step 3: Add rotation params + new declarations in `engine/physics/PhysicsWorld.h`**

Replace the `// --- Body creation ---` block:
```cpp
    BodyId createStaticBox     (Vec3 pos, Vec3 halfExtents);
    BodyId createDynamicBox    (Vec3 pos, Vec3 halfExtents, float mass);
    BodyId createDynamicSphere (Vec3 pos, float radius,         float mass);
    BodyId createDynamicCapsule(Vec3 pos, float halfHeight, float radius, float mass);
    void   destroyBody(BodyId);
```
with:
```cpp
    BodyId createStaticBox     (Vec3 pos, Vec3 halfExtents, Quat rotation = Quat::identity());
    BodyId createStaticSphere  (Vec3 pos, float radius,     Quat rotation = Quat::identity());
    BodyId createStaticCapsule (Vec3 pos, float halfHeight, float radius, Quat rotation = Quat::identity());
    BodyId createDynamicBox    (Vec3 pos, Vec3 halfExtents, float mass, Quat rotation = Quat::identity());
    BodyId createDynamicSphere (Vec3 pos, float radius,     float mass, Quat rotation = Quat::identity());
    BodyId createDynamicCapsule(Vec3 pos, float halfHeight, float radius, float mass, Quat rotation = Quat::identity());
    void   destroyBody(BodyId);
```

- [ ] **Step 4: Thread rotation through `createBodyImpl` + implement creators in `engine/physics/PhysicsWorld.cpp`**

Change `createBodyImpl`'s signature + the `BodyCreationSettings` line. Replace:
```cpp
BodyId createBodyImpl(PhysicsWorld::Impl& impl,
                      JPH::ShapeRefC shape,
                      JPH::RVec3 pos,
                      JPH::EMotionType motion,
                      JPH::ObjectLayer layer,
                      float mass) {
    JPH::BodyCreationSettings settings(shape, pos, JPH::Quat::sIdentity(), motion, layer);
```
with:
```cpp
BodyId createBodyImpl(PhysicsWorld::Impl& impl,
                      JPH::ShapeRefC shape,
                      JPH::RVec3 pos,
                      JPH::Quat rot,
                      JPH::EMotionType motion,
                      JPH::ObjectLayer layer,
                      float mass) {
    JPH::BodyCreationSettings settings(shape, pos, rot, motion, layer);
```

Replace the four existing creator bodies + add two static ones:
```cpp
BodyId PhysicsWorld::createStaticBox(Vec3 pos, Vec3 halfExtents, Quat rotation) {
    JPH::ShapeRefC shape = new JPH::BoxShape(toJ(halfExtents));
    return createBodyImpl(*impl_, shape, toJ(pos), toJ(rotation),
                          JPH::EMotionType::Static, Layers::NON_MOVING, 0.0f);
}

BodyId PhysicsWorld::createStaticSphere(Vec3 pos, float radius, Quat rotation) {
    JPH::ShapeRefC shape = new JPH::SphereShape(radius);
    return createBodyImpl(*impl_, shape, toJ(pos), toJ(rotation),
                          JPH::EMotionType::Static, Layers::NON_MOVING, 0.0f);
}

BodyId PhysicsWorld::createStaticCapsule(Vec3 pos, float halfH, float r, Quat rotation) {
    JPH::ShapeRefC shape = new JPH::CapsuleShape(halfH, r);
    return createBodyImpl(*impl_, shape, toJ(pos), toJ(rotation),
                          JPH::EMotionType::Static, Layers::NON_MOVING, 0.0f);
}

BodyId PhysicsWorld::createDynamicBox(Vec3 pos, Vec3 halfExtents, float mass, Quat rotation) {
    JPH::ShapeRefC shape = new JPH::BoxShape(toJ(halfExtents));
    return createBodyImpl(*impl_, shape, toJ(pos), toJ(rotation),
                          JPH::EMotionType::Dynamic, Layers::MOVING, mass);
}

BodyId PhysicsWorld::createDynamicSphere(Vec3 pos, float radius, float mass, Quat rotation) {
    JPH::ShapeRefC shape = new JPH::SphereShape(radius);
    return createBodyImpl(*impl_, shape, toJ(pos), toJ(rotation),
                          JPH::EMotionType::Dynamic, Layers::MOVING, mass);
}

BodyId PhysicsWorld::createDynamicCapsule(Vec3 pos, float halfH, float r, float mass, Quat rotation) {
    JPH::ShapeRefC shape = new JPH::CapsuleShape(halfH, r);
    return createBodyImpl(*impl_, shape, toJ(pos), toJ(rotation),
                          JPH::EMotionType::Dynamic, Layers::MOVING, mass);
}
```

- [ ] **Step 5: Build + run all tests**
```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target test_physics_world
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build; `test_physics_world` passes; full suite green.

- [ ] **Step 6: Commit**
```bash
git add engine/physics/PhysicsWorld.h engine/physics/PhysicsWorld.cpp tests/test_physics_world.cpp
git commit -m "M42: PhysicsWorld static sphere/capsule + rotation-at-create"
```

---

## Phase B — Data model + Inspector

### Task B1: `SceneEntity` optionals + SceneIO

**Files:**
- Modify: `engine/scene/SceneFormat.h`
- Modify: `engine/scene/SceneIO.cpp`
- Modify: `tests/test_scene_io.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_scene_io.cpp` a subtest covering save→load of an entity with both optionals + an entity without them. Match the file's existing pattern (it builds a `SceneFile`, registers types via `RegisterCoreTypes`, calls `saveSceneFile`/`loadSceneFile` to a temp path). Add `#include "world/CollisionShape.h"` and `#include "audio/AudioEmitter.h"`, ensure `registerCollisionShape(r)` + `registerAudioEmitter(r)` are called wherever the test sets up its `Reflection r`, then add:
```cpp
    // --- M42: collision + audio optionals round-trip ---
    {
        iron::SceneFile s;
        iron::SceneEntity withBoth;
        withBoth.name = "crate";
        withBoth.mesh.primitive = iron::PrimitiveKind::Cube;
        withBoth.collision = iron::CollisionShape{};
        withBoth.collision->body = iron::ColliderBody::Dynamic;
        withBoth.collision->mass = 7.0f;
        withBoth.audio = iron::AudioEmitter{};
        withBoth.audio->wavPath = "hum.wav";
        withBoth.audio->loop = true;
        s.entities.push_back(withBoth);

        iron::SceneEntity plain;
        plain.name = "floor";
        plain.mesh.primitive = iron::PrimitiveKind::Plane;
        s.entities.push_back(plain);

        const std::string path = "m42_sceneio_tmp.json";
        CHECK(iron::saveSceneFile(r, s, path));
        const auto loaded = iron::loadSceneFile(r, path);
        CHECK(loaded.has_value());
        CHECK(loaded->entities.size() == 2);

        const auto& a = loaded->entities[0];
        CHECK(a.collision.has_value());
        CHECK(a.collision->body == iron::ColliderBody::Dynamic);
        CHECK_NEAR(a.collision->mass, 7.0f);
        CHECK(a.audio.has_value());
        CHECK(a.audio->wavPath == "hum.wav");

        const auto& b = loaded->entities[1];
        CHECK(!b.collision.has_value());
        CHECK(!b.audio.has_value());
    }
```
(If `test_scene_io.cpp`'s `Reflection` is named differently or registration is centralized, adapt the two register calls accordingly.)

- [ ] **Step 2: Run to verify it fails**
```bash
cmake --build build-vk --config Debug --target test_scene_io
```
Expected: compile error — `SceneEntity` has no member `collision` / `audio`.

- [ ] **Step 3: Add the optionals to `engine/scene/SceneFormat.h`**

Add the includes near the top:
```cpp
#include "audio/AudioEmitter.h"
#include "world/CollisionShape.h"
```
Extend `SceneEntity`:
```cpp
struct SceneEntity {
    std::string name;
    Transform   transform;
    MeshRef     mesh;
    MaterialDef material;
    std::optional<CollisionShape> collision;  // M42 — absent = no collider
    std::optional<AudioEmitter>   audio;       // M42 — absent = no emitter
};
```

- [ ] **Step 4: Serialize the optionals in `engine/scene/SceneIO.cpp`**

In `entityToJson`, before `return j;`:
```cpp
    if (e.collision) j["collision"] = componentToJson(r, *e.collision);
    if (e.audio)     j["audio"]     = componentToJson(r, *e.audio);
```
In `entityFromJson`, before `return e;`:
```cpp
    if (j.contains("collision")) {
        e.collision = CollisionShape{};
        componentFromJson(r, *e.collision, j["collision"]);
    }
    if (j.contains("audio")) {
        e.audio = AudioEmitter{};
        componentFromJson(r, *e.audio, j["audio"]);
    }
```

- [ ] **Step 5: Build + run all tests**
```bash
cmake --build build-vk --config Debug --target ironcore
cmake --build build-vk --config Debug --target test_scene_io
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build; `test_scene_io` passes; full suite green.

- [ ] **Step 6: Commit**
```bash
git add engine/scene/SceneFormat.h engine/scene/SceneIO.cpp tests/test_scene_io.cpp
git commit -m "M42: SceneEntity collision/audio optionals + SceneIO round-trip"
```

---

### Task B2: Inspector — render-if-present + Add/Remove-Component combo

**Files:**
- Modify: `engine/editor/SceneInspector.cpp`

No automated test (pure ImGui); acceptance = builds clean and the panel behaves in the C/D visual gate.

- [ ] **Step 1: Implement the table-driven optional-component UI in `engine/editor/SceneInspector.cpp`**

Add includes at the top:
```cpp
#include "audio/AudioEmitter.h"
#include "world/CollisionShape.h"
```
Replace the entity-body block:
```cpp
    // Entity body — purely reflection-driven.
    changed |= renderComponent(reflection, e.transform);
    changed |= renderComponent(reflection, e.mesh);
    changed |= renderComponent(reflection, e.material);
```
with:
```cpp
    // Entity body — purely reflection-driven.
    changed |= renderComponent(reflection, e.transform);
    changed |= renderComponent(reflection, e.mesh);
    changed |= renderComponent(reflection, e.material);

    // M42: optional components (collision / audio). Table-driven so the combo +
    // render loop stay generic; a future optional component is one row here +
    // one std::optional field on SceneEntity. (A fully generic world.add<T>
    // combo waits for the World-migration milestone.)
    struct OptionalComp {
        const char* label;
        bool (*present)(const SceneEntity&);
        void (*attach)(SceneEntity&);
        void (*remove)(SceneEntity&);
        bool (*render)(const Reflection&, SceneEntity&);
    };
    static const OptionalComp kOptional[] = {
        { "CollisionShape",
          [](const SceneEntity& s){ return s.collision.has_value(); },
          [](SceneEntity& s){ s.collision.emplace(); },
          [](SceneEntity& s){ s.collision.reset(); },
          [](const Reflection& r, SceneEntity& s){ return renderComponent(r, *s.collision); } },
        { "AudioEmitter",
          [](const SceneEntity& s){ return s.audio.has_value(); },
          [](SceneEntity& s){ s.audio.emplace(); },
          [](SceneEntity& s){ s.audio.reset(); },
          [](const Reflection& r, SceneEntity& s){ return renderComponent(r, *s.audio); } },
    };

    for (const OptionalComp& oc : kOptional) {
        if (!oc.present(e)) continue;
        changed |= oc.render(reflection, e);
        ImGui::PushID(oc.label);
        if (ImGui::SmallButton("Remove")) { oc.remove(e); changed = true; }
        ImGui::PopID();
    }

    // "Add Component" combo lists only the optionals this entity lacks.
    if (ImGui::BeginCombo("Add Component", "Add Component ...")) {
        for (const OptionalComp& oc : kOptional) {
            if (oc.present(e)) continue;
            if (ImGui::Selectable(oc.label)) { oc.attach(e); changed = true; }
        }
        ImGui::EndCombo();
    }
```

- [ ] **Step 2: Build the editor lib + sandbox**
```bash
cmake --build build-vk --config Debug --target ironcore_editor
cmake --build build-vk --config Debug --target sandbox
```
Expected: clean build (benign `LNK4217` only).

- [ ] **Step 3: Run all tests (nothing should regress)**
```bash
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: full suite green.

- [ ] **Step 4: Commit**
```bash
git add engine/editor/SceneInspector.cpp
git commit -m "M42: Inspector Add/Remove-Component combo for collision/audio optionals"
```

---

## Phase C — Sandbox runtime

### Task C1: Build bodies + voices on Play; dynamic write-back; listener + voice tracking

**Files:**
- Modify: `games/11-sandbox/main.cpp`

Replaces M41's debug-cube spawn/despawn with real authored-component runtime. The M41 magenta debug cube + floor go away (Task C2 removes the render block; this task removes the bodies + spawn logic).

- [ ] **Step 1: Add includes + register the two new types**

Near the other `#include` lines, add:
```cpp
#include "audio/AudioEmitter.h"
#include "audio/AudioEngine.h"
#include "world/CollisionShape.h"
```
In the reflection-registration block (after `iron::registerRenderHandles(reflection);`):
```cpp
    iron::registerCollisionShape(reflection);
    iron::registerAudioEmitter(reflection);
```

- [ ] **Step 2: Construct an AudioEngine after the PhysicsWorld**

After the M41 `iron::PhysicsWorld physics; … physics.init() …` block (around line 452–456), add:
```cpp
    iron::AudioEngine audio;
    if (!audio.init())
        iron::Log::warn("sandbox: AudioEngine init failed; emitters will be silent");
    // WAV cache so re-entering Play doesn't reload from disk each time.
    std::unordered_map<std::string, iron::SoundHandle> soundCache;
    auto soundFor = [&](const std::string& relPath) -> iron::SoundHandle {
        if (relPath.empty()) return iron::kInvalidSound;
        auto it = soundCache.find(relPath);
        if (it != soundCache.end()) return it->second;
        const iron::SoundHandle h = audio.loadSound(exeDir + "/" + relPath);
        soundCache[relPath] = h;
        return h;
    };
```
(`<unordered_map>` is already pulled in transitively; if the build complains, add `#include <unordered_map>` near the top.)

- [ ] **Step 3: Replace the M41 debug-body state + spawn/despawn with runtime maps**

Delete the M41 block (lines ~461–484): the `debugCubeBody`/`debugFloorBody` declarations, `spawnDebugBodies`, and `despawnDebugBodies`. Replace with:
```cpp
    // M42: runtime bodies/voices built from authored components on Edit->Play.
    // Keyed by scene-entity index (parallel to scene.entities / sceneIndexToEntity).
    std::unordered_map<int, iron::BodyId>  playBodies;
    std::unordered_map<int, iron::VoiceId> playVoices;

    auto spawnRuntime = [&]() {
        for (int i = 0; i < static_cast<int>(scene.entities.size()); ++i) {
            const iron::SceneEntity& e = scene.entities[i];
            if (e.collision) {
                const iron::CollisionShape& cs = *e.collision;
                const iron::Vec3 p = e.transform.position;
                const iron::Quat q = e.transform.rotation;
                iron::BodyId b = iron::kInvalidBody;
                const bool dyn = (cs.body == iron::ColliderBody::Dynamic);
                switch (cs.shape) {
                    case iron::ColliderShape::Box:
                        b = dyn ? physics.createDynamicBox(p, cs.halfExtents, cs.mass, q)
                                : physics.createStaticBox (p, cs.halfExtents, q);
                        break;
                    case iron::ColliderShape::Sphere:
                        b = dyn ? physics.createDynamicSphere(p, cs.radius, cs.mass, q)
                                : physics.createStaticSphere (p, cs.radius, q);
                        break;
                    case iron::ColliderShape::Capsule:
                        b = dyn ? physics.createDynamicCapsule(p, cs.halfHeight, cs.radius, cs.mass, q)
                                : physics.createStaticCapsule (p, cs.halfHeight, cs.radius, q);
                        break;
                }
                if (b.isValid()) playBodies[i] = b;
            }
            if (e.audio && e.audio->playOnStart) {
                const iron::AudioEmitter& em = *e.audio;
                const iron::SoundHandle h = soundFor(em.wavPath);
                if (h != iron::kInvalidSound) {
                    if (em.loop && em.spatial) {
                        const iron::VoiceId v = audio.playLooping(h, e.transform.position, em.gain);
                        if (v != iron::kInvalidVoice) playVoices[i] = v;
                    } else if (em.spatial) {
                        audio.playSoundAt(h, e.transform.position, em.gain);  // one-shot
                    } else {
                        audio.playSoundLocal(h, em.gain);                     // 2D one-shot
                    }
                }
            }
        }
    };

    auto despawnRuntime = [&]() {
        for (auto& [idx, body] : playBodies) physics.destroyBody(body);
        for (auto& [idx, voice] : playVoices) audio.stop(voice);
        playBodies.clear();
        playVoices.clear();
    };
```

- [ ] **Step 4: Point `togglePlayMode` at the new spawn/despawn**

In `togglePlayMode` (lines ~486–504) replace `despawnDebugBodies();` with `despawnRuntime();` and `spawnDebugBodies();` with `spawnRuntime();`. (Snapshot/restore lines stay exactly as M41 wrote them — `editScene = scene; …` etc.)

- [ ] **Step 5: Per-frame listener + dynamic write-back + voice tracking**

In setUpdate, replace the M41 physics-step block:
```cpp
        // M41: physics runs only in Play mode.
        if (editor.isPlaying()) {
            physics.step(t.deltaSeconds);
        }
```
with:
```cpp
        // M42: audio listener follows the camera (forward from cam; world-up).
        audio.setListener(cam.position, cam.forward(), iron::Vec3{0.0f, 1.0f, 0.0f});

        // M41/M42: physics runs only in Play mode; dynamic bodies write their
        // pose back into scene.entities (the unconditional scene->World mirror
        // in setRender then propagates it to the renderer). Static bodies and
        // non-collider entities are untouched. Snapshot restores all of this on Stop.
        if (editor.isPlaying()) {
            physics.step(t.deltaSeconds);
            for (auto& [idx, body] : playBodies) {
                if (idx < 0 || idx >= static_cast<int>(scene.entities.size())) continue;
                if (scene.entities[idx].collision &&
                    scene.entities[idx].collision->body == iron::ColliderBody::Dynamic) {
                    scene.entities[idx].transform.position = physics.bodyPosition(body);
                    scene.entities[idx].transform.rotation = physics.bodyRotation(body);
                    auto vit = playVoices.find(idx);
                    if (vit != playVoices.end())
                        audio.setVoicePosition(vit->second, scene.entities[idx].transform.position);
                }
            }
        }
```

- [ ] **Step 6: Build + run all tests**
```bash
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build; full suite green. (Don't launch yet — Task C2 adds the collider wireframes, then D1 is the visual gate.)

- [ ] **Step 7: Commit**
```bash
git add games/11-sandbox/main.cpp
git commit -m "M42: sandbox runtime — build bodies/voices on Play, dynamic write-back, listener + voice tracking"
```

---

### Task C2: Edit-mode collider wireframes (replace the M41 debug-cube render)

**Files:**
- Modify: `games/11-sandbox/main.cpp`

- [ ] **Step 1: Add a collider-wireframe helper**

Near the other render-helper lambdas in `main()` (e.g. just after `gizmoOriginFor`, around line 561), add:
```cpp
    // M42: draw an entity's collider as a green wireframe in Edit mode, so the
    // user can see what they're authoring. Box = 12 edges; Sphere = 3 rings;
    // Capsule = cylinder approximation (2 rings + 4 verticals). Uses the same
    // drawLineOverlay path as the selection outline.
    auto drawColliderWireframe = [&](const iron::SceneEntity& e) {
        if (!e.collision) return;
        const iron::CollisionShape& cs = *e.collision;
        const iron::Vec3 c  = e.transform.position;
        const iron::Vec3 col{0.2f, 1.0f, 0.3f};  // collider green
        const iron::Quat q  = e.transform.rotation;
        // Rotate a local offset into world space (collider ignores entity scale).
        auto wp = [&](float x, float y, float z) {
            const iron::Vec4 r = q.toMat4() * iron::Vec4{x, y, z, 1.0f};
            return iron::Vec3{c.x + r.x, c.y + r.y, c.z + r.z};
        };
        auto ring = [&](int axis, float r, float h) {  // circle of radius r at offset h along `axis`
            constexpr int N = 24;
            iron::Vec3 prev{};
            for (int i = 0; i <= N; ++i) {
                const float a = (static_cast<float>(i) / N) * 2.0f * std::numbers::pi_v<float>;
                const float u = r * std::cos(a), v = r * std::sin(a);
                iron::Vec3 cur = (axis == 0) ? wp(h, u, v)
                               : (axis == 1) ? wp(u, h, v)
                                             : wp(u, v, h);
                if (i > 0) renderer.drawLineOverlay(prev, cur, col);
                prev = cur;
            }
        };
        switch (cs.shape) {
            case iron::ColliderShape::Box: {
                const iron::Vec3 h = cs.halfExtents;
                iron::Vec3 v[8];
                for (int i = 0; i < 8; ++i)
                    v[i] = wp((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
                const int edges[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},
                                          {4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
                for (auto& ed : edges) renderer.drawLineOverlay(v[ed[0]], v[ed[1]], col);
                break;
            }
            case iron::ColliderShape::Sphere:
                ring(0, cs.radius, 0.0f);
                ring(1, cs.radius, 0.0f);
                ring(2, cs.radius, 0.0f);
                break;
            case iron::ColliderShape::Capsule: {
                const float hh = cs.halfHeight, r = cs.radius;
                ring(1, r,  hh);   // top cap ring
                ring(1, r, -hh);   // bottom cap ring
                renderer.drawLineOverlay(wp( r, -hh, 0), wp( r, hh, 0), col);
                renderer.drawLineOverlay(wp(-r, -hh, 0), wp(-r, hh, 0), col);
                renderer.drawLineOverlay(wp(0, -hh,  r), wp(0, hh,  r), col);
                renderer.drawLineOverlay(wp(0, -hh, -r), wp(0, hh, -r), col);
                break;
            }
        }
    };
```
(`std::numbers` + `<cmath>` are already included in this file — the M40 view-gizmo + zoom code uses both.)

- [ ] **Step 2: Replace the M41 magenta debug-cube render block**

Delete the entire M41 block (lines ~1012–1048, the `if (editor.isPlaying() && debugCubeBody.isValid()) { … magenta … floorMagenta … }`) and replace with:
```cpp
        // M42: collider wireframes (green) in Edit mode, for every entity that
        // has a CollisionShape. Hidden in Play mode (the moving meshes show the
        // result). Drawn via the same drawLineOverlay path as the outline.
        if (!editor.isPlaying()) {
            for (const auto& e : scene.entities) drawColliderWireframe(e);
        }
```

- [ ] **Step 3: Build + run all tests**
```bash
cmake --build build-vk --config Debug --target sandbox
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: clean build; full suite green.

- [ ] **Step 4: Commit**
```bash
git add games/11-sandbox/main.cpp
git commit -m "M42: Edit-mode collider wireframes (replace M41 debug-cube render)"
```

---

## Phase D — Verification + PR + merge

### Task D1: Visual gate + push + PR + squash-merge + memory

- [ ] **Step 1: Full clean build + tests**
```bash
cmake --build build-vk --config Debug
ctest --test-dir build-vk -C Debug --output-on-failure
```
Expected: 54/54 green (52 baseline + test_collision_shape + test_audio_emitter; test_scene_io/physics_world/audio_engine gained subtests, not new CTest entries).

- [ ] **Step 2: User visual gate**

Hand back to the user with this checklist:
```
.\build-vk\games\11-sandbox\Debug\sandbox.exe
```

| Action | Expected |
|---|---|
| Select a cube; Inspector → "Add Component" combo → CollisionShape | A CollisionShape section appears (shape/body/halfExtents/radius/halfHeight/mass) + a Remove button. A green wireframe box appears around the cube. |
| Set its `body` to Dynamic | (no visible change in Edit) |
| Select the floor plane; Add CollisionShape; leave body = Static | Green wireframe around the floor. |
| Press Play (F5) | The Dynamic cube falls and lands on the Static floor (the actual mesh moves, not a debug wireframe). Collider wireframes hidden in Play. |
| Press Stop (F5 / Esc) | Cube returns to its authored position; collider wireframes reappear. |
| Add an AudioEmitter to the cube (set `wavPath` to a mono WAV under the exe's assets, e.g. an existing CC0 sfx), loop = true | Inspector shows the AudioEmitter fields + Remove. |
| Play | The looping sound plays, panning as you move the camera (RMB-look / WASD), and follows the cube as it falls. |
| Stop | Sound stops. |
| Change shape to Sphere / Capsule on some entity | Green ring / capsule wireframe; Play → it collides as that shape. |
| Outliner → Save; reopen the app | The colliders + emitter persist (written into demo.json). |
| Remove a component via its Remove button | Section + wireframe disappear. |

If any row regresses, return to the owning task (C1 runtime, C2 wireframes, B2 Inspector, B1 IO) and fix before proceeding.

- [ ] **Step 3: Push the branch**
```bash
git push -u origin feat/m42-collision-audio
```

- [ ] **Step 4: Open the PR**

Create `tmp/m42-pr-body.md`:
```markdown
## Summary

M42 — CollisionShape + AudioEmitter authorable components. Completes the editor's "place objects, add collision, set audio" loop, exercised in M41 Play mode.

- **Two POD components** added as `std::optional` fields on `iron::SceneEntity`: `CollisionShape` (Box/Sphere/Capsule × Static/Dynamic + size + mass) and `AudioEmitter` (wavPath/gain/loop/spatial/playOnStart). Reflection sidecars register both + their enums.
- **Serialization + Inspector widgets are automatic** via the M38/M39 reflection layer — no `ReflectionIO`/`ReflectionInspector` engine changes (Bool/Float/String/Vec3/Enum all already supported). SceneIO gains `collision`/`audio` keys gated on the optionals (additive; existing scenes load unchanged).
- **Inspector Add/Remove-Component combo** — table-driven over the optional components; generic UI, one row per component.
- **`AudioEngine` looping-voice API** — `VoiceId` + `playLooping` / `stop` / `setVoicePosition`; looping voices reserve a source (excluded from one-shot voice-stealing). One-shot paths unchanged.
- **`PhysicsWorld`** — added static sphere/capsule creators + rotation-at-create (trailing default arg; existing callers unchanged).
- **Sandbox runtime** — on Edit→Play, builds Jolt bodies + audio voices from authored components; dynamic bodies write their pose back into `scene.entities` (the existing scene→World mirror propagates to the renderer) so meshes fall/move; the camera drives the audio listener; looping voices track dynamic positions. Stop tears down bodies/voices; M41's snapshot restores transforms. The M41 magenta debug cube is retired. Edit mode draws green collider wireframes.
- **Tests**: new `test_collision_shape`, `test_audio_emitter`; `test_scene_io` / `test_physics_world` / `test_audio_engine` gained subtests. 52 → 54 CTest entries.

## Test plan
- [x] Full suite green (54/54)
- [x] ironcore + ironcore_editor + sandbox build clean
- [x] Visual: add collider → Play → dynamic mesh falls onto static floor; add looping emitter → positional sound follows the object; Stop restores; Save persists.

## Known v1 limits (intentional, deferred)
- Kinematic bodies; physics materials (restitution/friction); per-emitter rolloff.
- Trigger/sensor volumes + contact events to script.
- Generic `world.add<T>` Add-Component combo (needs World migration).
- Collider auto-fit from mesh bounds (defaults to unit sizes; user resizes in Inspector).
- Capsule wireframe is a cylinder approximation. Many looping emitters can exhaust the 32-source pool (warn-once).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```
Open it:
```bash
gh pr create --title "M42: CollisionShape + AudioEmitter authorable components" --body-file tmp/m42-pr-body.md
```

- [ ] **Step 5: Watch CI**
```bash
gh pr checks --watch
```
Expected: `Build & test (Windows / MSVC)` passes (~6 min warm cache). On transient vcpkg/Kitware flake, `gh run rerun <id> --failed`.

- [ ] **Step 6: Squash-merge**
```bash
gh pr merge --squash --delete-branch
git checkout main && git pull --ff-only origin main
git log --oneline -3
```

- [ ] **Step 7: Update memory**

After merge, update:
- `MEMORY.md` index `iron-core-engine-progress` line: bump to M42 merged (PR #, SHA) + note next options drop M42 and surface the editor-UI overhaul ([[editor-ui-overhaul-direction]]) + World migration as the front-runners.
- `iron-core-engine-progress.md`: append an `M42 — CollisionShape + AudioEmitter` entry (merge SHA, one-paragraph summary, the v1 limits list, note that the AudioEngine `VoiceId` API + PhysicsWorld static-sphere/capsule+rotation landed here as reusable engine surface).
- `iron-core-engine-roadmap.md`: mark M42 done; update the "next options after M41" line to drop M42.

---

## Acceptance criteria

1. `CollisionShape` + `AudioEmitter` exist as POD components with reflection sidecars; both round-trip through `ReflectionIO` (all fields + enums) — unit-tested.
2. `SceneEntity` carries `std::optional<CollisionShape> collision` + `std::optional<AudioEmitter> audio`; SceneIO writes/reads them (omitted when absent); existing `demo.json` still loads — unit-tested.
3. `AudioEngine::playLooping` returns a `VoiceId`; `stop`/`setVoicePosition` work; looping voices aren't stolen by one-shot playback; all are safe no-ops on an uninitialized engine — unit-tested.
4. `PhysicsWorld` can create static spheres/capsules and bodies with a non-identity rotation; existing callers compile unchanged — unit-tested.
5. Inspector renders present collision/audio components, offers Add Component for absent ones, and Remove for present ones.
6. In Play: Dynamic colliders fall/collide and move their entity's mesh; Static colliders are immovable; emitters play (looping/positional, following dynamic entities). Stop restores authored transforms + stops voices.
7. Edit mode shows green collider wireframes (box/sphere/capsule); hidden in Play.
8. 52 → 54 CTest cases green; renderer / shipping games / reflection-layer engine code untouched.

---

## Risk log

- **Write-back vs. the unconditional scene→World mirror.** Dynamic write-back MUST target `scene.entities[i].transform` (not World directly) — the setRender mirror at lines ~928–939 copies `scene → World` every frame and would clobber a direct World write. Verified during planning; encoded in C1 Step 5.
- **Quaternion comparison in tests.** A4's rotation test compares via `|dot| ≈ 1` to be double-cover safe; don't compare components directly.
- **Voice pool exhaustion.** >32 simultaneous looping emitters exhaust the pool (`playLooping` warns + returns `kInvalidVoice`). Acceptable v1; priority/virtualization is future work.
- **`test_scene_io` / `test_physics_world` / `test_audio_engine` structure.** These existing files may register types or assert differently than assumed; adapt the appended subtests to the file's actual harness (register the two new types where the test builds its `Reflection`).
- **Capsule wireframe fidelity.** Cylinder approximation (no hemisphere arcs) — an authoring affordance, not a debug-exact shape. Flagged in the PR limits.
- **Rotation + dynamic write-back interaction.** Bodies are created with the authored rotation (A4) so the first write-back frame doesn't snap rotation to identity. If a future component applies entity *scale* to colliders, note Jolt shapes ignore the entity scale here (colliders are sized by their own fields).
