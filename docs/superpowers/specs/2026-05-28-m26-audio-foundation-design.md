# M26 — Audio Foundation (Design)

**Date:** 2026-05-28
**Status:** Approved
**Predecessors:** M20 (Jolt projectiles), M21 (death-into-ragdoll), M25 (skinned characters)
**Successors:** M27 (SFX pass: footsteps + gunshots + hit + jump)

## Goal

Bring positional 3D audio into the engine. Smallest meaningful scope: when a rocket detonates in `net-shooter`, every peer hears the explosion coming from the correct world position. Proves the full pipeline (library init → WAV decode → 3D source playback → per-frame listener update). Content (more SFX) lands in M27.

## Non-Goals

- **OGG/MP3 support** — WAV only in v1; M27 adds `stb_vorbis` for compressed assets.
- **Custom attenuation curves** — OpenAL defaults are fine for one short SFX.
- **Doppler effect** — sources don't carry velocity in v1.
- **Looping sounds** — footsteps tied to walk/run state belong to M27.
- **Audio buses / mixer / volume groups** — single master gain in v1.
- **Hot-reload of audio files** — folded into the general asset hot-reload milestone.
- **UI sounds / music streaming** — production-scope, not foundation.

## Library Choice

**OpenAL Soft** via `vcpkg`. Industry-standard 3D positional audio, MIT licensed, mature, well-understood pattern: device → context → buffer → source → play. Chosen over miniaudio (less battle-tested 3D spatializer) and FMOD/Wwise (heavy middleware, overkill).

**WAV decoder:** `dr_wav.h` from the dr_libs collection (single header, public domain). Vendored under `third_party/dr_libs/`. No vcpkg dep needed; included only inside `AudioEngine.cpp` with `#define DR_WAV_IMPLEMENTATION`.

## Architecture

### `iron::AudioEngine` (new engine type)

Files:
- `engine/audio/AudioEngine.h`
- `engine/audio/AudioEngine.cpp`

```cpp
#pragma once

#include "math/Vec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iron {

using SoundHandle = std::uint32_t;
constexpr SoundHandle kInvalidSound = 0;

// One audio device + context, a per-engine pool of OpenAL sources for
// concurrent playback, and a registry of loaded sound buffers. Non-copyable.
//
// Lifecycle: construct → init() once at game startup → use for the lifetime
// of the app → shutdown() on exit (also called by destructor). Safe to call
// playSoundAt / setListener every frame.
class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Open the default device + context, allocate the source pool.
    // Returns false on any failure (no device, no context, no sources). The
    // engine then operates as a no-op — loadSound / playSoundAt are safe to
    // call and silently do nothing. Logs the failure once via Log::warn.
    bool init();

    // Release sources, buffers, context, device. Idempotent; called by dtor.
    void shutdown();

    // Decode a WAV file from disk into an OpenAL buffer and return a handle.
    // Supports mono and stereo, 8/16/24-bit PCM, IEEE float. Stereo files
    // play non-positionally (OpenAL spec) — a warn-once nudges the caller
    // to ship mono for 3D SFX. Returns kInvalidSound on failure.
    SoundHandle loadSound(const std::string& wavPath);

    // Play `h` at `worldPos` with `gain` (1.0 = full). Returns immediately;
    // the sound plays asynchronously through OpenAL's internal mixer.
    // If all sources are busy, the oldest non-looping source is reclaimed.
    // No-op if `h == kInvalidSound` or the engine failed to init.
    void playSoundAt(SoundHandle h, Vec3 worldPos, float gain = 1.0f);

    // Update the listener (camera) state. Call once per frame before any
    // playSoundAt calls. `forward` and `up` are unit vectors describing the
    // listener's orientation (matches the rendering camera).
    void setListener(Vec3 cameraPos, Vec3 forward, Vec3 up);

    bool initialized() const { return initialized_; }

private:
    // ALCdevice* / ALCcontext* / std::vector<ALuint> source pool / buffer
    // registry. Forward-declared via pimpl OR opaque void* fields to keep
    // OpenAL headers out of the public Audio API. Pick whichever pattern
    // matches the rest of the engine (PhysicsWorld uses pimpl).
};

}  // namespace iron
```

**Source pool sizing:** 32 sources. Each source can play one buffer at a time. With ~5-10 concurrent peers each firing rockets/hitscan and footsteps coming in M27, 32 is comfortable headroom. If exhausted, voice-steal the oldest (warn once per app run).

**Stereo handling:** OpenAL plays stereo sources non-positionally (spec mandates this — it has nowhere to apply 3D math). Loading a stereo file is legal but the result is "play with full gain, no positioning". Useful for UI sounds later. SFX in v1 ship mono.

### Net-Shooter Wiring

Single instance owned by the game's main scope:

```cpp
iron::AudioEngine audio;
if (!audio.init()) {
    iron::Log::warn("net-shooter: audio init failed; running silent");
}
const iron::SoundHandle boomSfx = audio.loadSound("assets/sfx/rocket-explode.wav");
```

Each frame, before rendering:

```cpp
audio.setListener(cameraPos, lookForward, iron::Vec3{0, 1, 0});
```

(`cameraPos` and `lookForward` already exist in the game's render path — they drive the view matrix.)

On rocket detonation, both host and client paths already have a point of contact:

- **Host:** when the host's Jolt physics tick detects a rocket hit, it broadcasts `DespawnProjectileMsg{projectileId, x, y, z}` (already in the wire format from M20). The host can play the sound at the impact point at the same call site.
- **Client:** when `DespawnProjectileMsg` is handled, the existing FX spawn code already runs at `msg.x/y/z`. Play the sound there.

```cpp
audio.playSoundAt(boomSfx, iron::Vec3{msg.x, msg.y, msg.z}, /*gain=*/1.0f);
```

No new network packets, no protocol bump. The despawn message already carries the impact position.

## Listener Convention

**Listener position = camera position**, not player foot position. Matches what the player visually sees. First-person camera is at eye height (~1.7m above feet) which gives the natural "I'm hearing from my own ears" effect.

**Forward vector**: same vector the camera uses to compute the view matrix (the existing `lookForward = Vec3{-sin(yaw)·cos(pitch), -sin(pitch), -cos(yaw)·cos(pitch)}`).

**Up vector**: world Y (`{0, 1, 0}`).

This convention applies to host and clients equally. The host hears from its own camera; each client hears from its own camera.

## Dependencies

**`vcpkg.json`** — add the `openal-soft` port:

```json
{
  "dependencies": [
    "...",
    "openal-soft"
  ]
}
```

`openal-soft` is the OpenAL implementation; the header `<AL/al.h>` and `<AL/alc.h>` come with it.

**`third_party/dr_libs/dr_wav.h`** — vendor the single header (public domain). One-file download from https://github.com/mackron/dr_libs.

## Asset

One WAV: `games/07-net-shooter/assets/sfx/rocket-explode.wav`.

Source: CC0 explosion from freesound.org or opengameart.org. Must be mono, ~1-2s, 16-bit PCM, 44.1 or 48 kHz. The implementer picks a specific candidate (any CC0 explosion works); we capture the source attribution in the commit message.

A second tiny WAV at `tests/assets/sfx/test-beep.wav` (a 0.1s sine sweep, generated or vendored) for the smoke test.

## Files Changed

**Create:**
- `engine/audio/AudioEngine.h`
- `engine/audio/AudioEngine.cpp`
- `third_party/dr_libs/dr_wav.h` (vendored single header)
- `third_party/dr_libs/CMakeLists.txt` (header-only INTERFACE target, mirrors `tinygltf` if applicable)
- `tests/test_audio_engine.cpp` — init/shutdown smoke; WAV load smoke against `test-beep.wav`. Skip 3D playback tests (no audio device on CI).
- `tests/assets/sfx/test-beep.wav`
- `games/07-net-shooter/assets/sfx/rocket-explode.wav`

**Modify:**
- `vcpkg.json` — add `openal-soft`.
- `engine/CMakeLists.txt` — add audio sources; link OpenAL; add `third_party/dr_libs` INTERFACE.
- `tests/CMakeLists.txt` — register `test_audio_engine`.
- `games/07-net-shooter/main.cpp` — instantiate AudioEngine, load boomSfx, set listener per frame, play at rocket-despawn site (both host and client paths).
- `games/07-net-shooter/CMakeLists.txt` — the existing per-game asset-copy command already covers the new `sfx/` directory; no change needed unless that command excludes it (verify).
- `docs/engine/asset-pipeline.md` — append M26 section.

## Test Plan

**Unit:**
- `test_audio_engine.cpp`:
  - Init succeeds OR gracefully fails (env-dependent — skip on `IRON_CI=1`).
  - `loadSound` returns a non-zero handle for the test WAV.
  - `loadSound` returns `kInvalidSound` for a nonexistent path (no crash).
  - `playSoundAt(kInvalidSound, ...)` is a no-op (no crash).
  - `playSoundAt` on an un-init'd engine is a no-op (no crash).

**Integration / visual:**
- Build host + client, both connect.
- Fire a rocket. On detonation, both peers hear an explosion roughly from the impact direction (move the camera before firing to verify 3D positioning — sound should pan accordingly).
- Repeat for 10+ rockets in quick succession; no clipping, no source exhaustion crashes.

## Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| OpenAL device init fails on headless CI | `init()` returns false; engine becomes no-op; logs warning once. `IRON_CI=1` short-circuits in tests. |
| dr_wav single-header inflates compile time | Included only in `AudioEngine.cpp` with `DR_WAV_IMPLEMENTATION`. Other TUs don't see it. |
| Source exhaustion under heavy fire | Voice-steal the oldest non-looping source; warn-once. 32 sources is generous for M26. |
| WAV format quirks (24-bit, float, weird channel counts) | dr_wav handles the common cases; `loadSound` returns `kInvalidSound` and logs on unsupported formats. |
| Cross-platform OpenAL ABI | vcpkg pins the version per the manifest lock; CI builds match local. |

## Verification Command

```powershell
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
# second terminal:
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --client 127.0.0.1
```

Expected: rockets fired by either peer produce a positional boom on both screens.
