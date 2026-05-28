# M26: Audio Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring positional 3D audio into the engine and play one explosion sound at the rocket-detonation site in `net-shooter`, on both host and client. Proves library init → WAV decode → 3D playback → per-frame listener update end-to-end.

**Architecture:** New `iron::AudioEngine` engine type wraps OpenAL Soft (vcpkg `openal-soft`). WAV decoding via vendored single-header `dr_wav.h` (public domain). Per-engine ring of 32 sources reused for concurrent playback with voice-stealing on exhaustion. Net-shooter creates one `AudioEngine`, loads `rocket-explode.wav` once, sets the listener to the camera each frame, and calls `playSoundAt(boom, impactPos)` at the three existing rocket-despawn sites (one client handler + two host broadcast paths).

**Tech Stack:** C++23 (`/std:c++latest`), OpenAL Soft via vcpkg, dr_wav single header, CMake, CTest. Builds on M20's `DespawnProjectileMsg` (already carries impact position; zero new network packets).

**Spec:** `docs/superpowers/specs/2026-05-28-m26-audio-foundation-design.md`

**Prerequisite:** M25 merged on `main` (verified — commit `d2d05a9`). Branch already exists as `feat/m26-audio-foundation` with the spec committed.

---

## File Structure

**Create:**
- `engine/audio/AudioEngine.h` — public `iron::AudioEngine` interface, no OpenAL headers leaked.
- `engine/audio/AudioEngine.cpp` — OpenAL device/context/source-pool/buffer-registry; dr_wav decode.
- `third_party/dr_libs/dr_wav.h` — vendored single header (public domain).
- `third_party/dr_libs/CMakeLists.txt` — INTERFACE target exposing the include directory.
- `tests/test_audio_engine.cpp` — init smoke, WAV load smoke, error-path safety (no crashes on invalid handles).
- `tests/assets/sfx/test-beep.wav` — tiny synthesized WAV (mono, 16-bit PCM, ~0.1s).
- `games/07-net-shooter/assets/sfx/rocket-explode.wav` — CC0 explosion sample.

**Modify:**
- `vcpkg.json` — add `openal-soft` to dependencies.
- `third_party/CMakeLists.txt` — add `add_subdirectory(dr_libs)` next to the existing `stb` entry.
- `engine/CMakeLists.txt` — add `audio/AudioEngine.cpp` to sources; `find_package(OpenAL CONFIG REQUIRED)`; link OpenAL PRIVATE; link `dr_libs` (INTERFACE) PRIVATE.
- `tests/CMakeLists.txt` — register `test_audio_engine`.
- `games/07-net-shooter/main.cpp` — instantiate `AudioEngine`, `loadSound`, per-frame `setListener`, and `playSoundAt` at the three despawn sites.
- `docs/engine/asset-pipeline.md` — append M26 section.

**Out of scope (explicit non-goals per the spec):** OGG support, custom attenuation curves, doppler, looping sounds, audio buses/mixer, hot-reload, UI sounds/music. Each is deferred to M27+ explicitly.

---

## Task 1: Vendor `dr_wav.h`

**Files:**
- Create: `third_party/dr_libs/dr_wav.h`
- Create: `third_party/dr_libs/CMakeLists.txt`
- Modify: `third_party/CMakeLists.txt`

- [ ] **Step 1: Download `dr_wav.h`**

Use PowerShell:

```powershell
$dest = 'C:\Users\elias\Documents\_dev\iron-core-engine\third_party\dr_libs'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$src  = 'https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h'
Invoke-WebRequest -Uri $src -OutFile (Join-Path $dest 'dr_wav.h')
```

Verify the file is ~250 KB and starts with `/*\nWAV audio loader and writer.`:

```powershell
Get-Item (Join-Path $dest 'dr_wav.h') | Select-Object Length
Get-Content (Join-Path $dest 'dr_wav.h') -TotalCount 3
```

Expected: length around 250000-350000 bytes; first lines look like a license/header banner.

- [ ] **Step 2: Create `third_party/dr_libs/CMakeLists.txt`**

```cmake
# Header-only single-file libraries from https://github.com/mackron/dr_libs
# Currently used for: dr_wav.h (WAV decode in AudioEngine).
# Public-domain license; no link step required.
add_library(dr_libs INTERFACE)
target_include_directories(dr_libs INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
```

- [ ] **Step 3: Register the subdir in `third_party/CMakeLists.txt`**

The current contents (read first):

```cmake
add_library(stb_image STATIC stb/stb_image.cpp)
target_compile_options(stb_image PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W0>)
target_include_directories(stb_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stb)
```

Append:

```cmake

add_subdirectory(dr_libs)
```

- [ ] **Step 4: Reconfigure CMake and confirm the target exists**

```powershell
cmake --preset vk
```

Expected: configure succeeds with no errors mentioning `dr_libs` (target is INTERFACE so it won't appear in the build until something links it).

- [ ] **Step 5: Commit**

```bash
git add third_party/dr_libs/dr_wav.h third_party/dr_libs/CMakeLists.txt third_party/CMakeLists.txt
git commit -m "M26 Task 1: vendor dr_wav.h single-header WAV decoder"
```

---

## Task 2: Add `openal-soft` to vcpkg + declare `AudioEngine`

**Files:**
- Modify: `vcpkg.json`
- Create: `engine/audio/AudioEngine.h`
- Modify: `engine/CMakeLists.txt`

- [ ] **Step 1: Add openal-soft to `vcpkg.json`**

Current file:

```json
{
  "name": "iron-core-engine",
  "version-string": "0.0.0",
  "dependencies": [
    "gamenetworkingsockets",
    "joltphysics",
    "tinygltf",
    "vulkan-headers",
    "vulkan-loader",
    "glslang",
    "vulkan-memory-allocator"
  ]
}
```

Add `"openal-soft"` to the `dependencies` array (preserve alphabetical-ish ordering or just append — match what's there). Final:

```json
{
  "name": "iron-core-engine",
  "version-string": "0.0.0",
  "dependencies": [
    "gamenetworkingsockets",
    "joltphysics",
    "openal-soft",
    "tinygltf",
    "vulkan-headers",
    "vulkan-loader",
    "glslang",
    "vulkan-memory-allocator"
  ]
}
```

- [ ] **Step 2: Create `engine/audio/AudioEngine.h`**

```cpp
#pragma once

#include "math/Vec.h"

#include <cstdint>
#include <memory>
#include <string>

namespace iron {

using SoundHandle = std::uint32_t;
constexpr SoundHandle kInvalidSound = 0;

// One audio device + context, a per-engine pool of OpenAL sources for
// concurrent playback, and a registry of loaded sound buffers.
// Non-copyable; move-disabled to keep the underlying pimpl simple.
//
// Lifecycle:
//   construct -> init() once at startup -> use for the app's lifetime ->
//   shutdown() on exit (also called by the destructor).
//
// If init() fails (no audio device on a headless box, etc.) the engine
// becomes a silent no-op: loadSound / playSoundAt / setListener are all
// still safe to call but do nothing.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Open the default device + context, allocate the source pool (32).
    // Returns false on failure (logs once via Log::warn). The engine then
    // operates as a no-op.
    bool init();

    // Release sources, buffers, context, device. Idempotent.
    void shutdown();

    // Decode a WAV file from disk into an OpenAL buffer and return a
    // handle. Supports mono and stereo, 8/16/24-bit PCM, IEEE float.
    // Stereo files play non-positionally (OpenAL spec) — a warn-once
    // logs a nudge to ship mono for 3D SFX.
    // Returns kInvalidSound on any failure.
    SoundHandle loadSound(const std::string& wavPath);

    // Play `h` at `worldPos` with `gain` (1.0 = full).
    // If all sources are busy, voice-steals the oldest non-looping source.
    // No-op if `h == kInvalidSound` or the engine failed to init.
    void playSoundAt(SoundHandle h, Vec3 worldPos, float gain = 1.0f);

    // Update the listener (camera) state. Call once per frame before any
    // playSoundAt calls. `forward` and `up` are unit vectors describing
    // the listener's orientation; match the rendering camera.
    void setListener(Vec3 cameraPos, Vec3 forward, Vec3 up);

    bool initialized() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iron
```

- [ ] **Step 3: Link OpenAL in `engine/CMakeLists.txt`**

Read the existing structure — `ironcore` is declared with PUBLIC/PRIVATE link groups. Add:

After the `target_link_libraries(ironcore PRIVATE Jolt::Jolt)` line (~line 42-43), add a new block:

```cmake
# M26 — audio: OpenAL Soft (via vcpkg) + dr_wav (vendored header-only).
# OpenAL headers are PRIVATE to keep the engine's public API free of
# <AL/al.h>; AudioEngine.h uses opaque pimpl.
find_package(OpenAL CONFIG REQUIRED)
target_link_libraries(ironcore PRIVATE OpenAL::OpenAL dr_libs)
```

Also add `audio/AudioEngine.cpp` to the `add_library(ironcore STATIC ...)` source list — slot it alphabetically after `asset/CharacterAnimator.cpp` (i.e., before `core/` if alphabetical, or wherever makes sense in the existing order).

Modify the `add_library` source list to include the new line:

```cmake
add_library(ironcore STATIC
  ...
  asset/CharacterAnimator.cpp
  audio/AudioEngine.cpp
  ...
)
```

Note that AudioEngine.cpp doesn't exist yet (Task 3 creates it) — CMake configure will succeed but build will fail until Task 3. That's expected.

- [ ] **Step 4: Reconfigure and verify vcpkg installs openal-soft**

```powershell
cmake --preset vk
```

Expected: configure runs `vcpkg install openal-soft` (first time only — several minutes), then completes. Look for `Found OpenAL` in the output. If `find_package(OpenAL CONFIG REQUIRED)` fails, the vcpkg port might use a different config name; try `find_package(OpenAL REQUIRED)` (without CONFIG) — the openal-soft vcpkg port traditionally supports both.

- [ ] **Step 5: Commit**

```bash
git add vcpkg.json engine/audio/AudioEngine.h engine/CMakeLists.txt
git commit -m "M26 Task 2: declare AudioEngine + add openal-soft to vcpkg"
```

---

## Task 3: Implement `AudioEngine` + tests (TDD)

**Files:**
- Create: `engine/audio/AudioEngine.cpp`
- Create: `tests/test_audio_engine.cpp`
- Create: `tests/assets/sfx/test-beep.wav`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Synthesize a tiny test WAV**

Write a one-shot PowerShell script that generates a 0.1s 440 Hz sine wave at 44.1 kHz, 16-bit mono. Save it as `tests/assets/sfx/test-beep.wav`. Pure PowerShell, no external tools:

```powershell
$path = 'C:\Users\elias\Documents\_dev\iron-core-engine\tests\assets\sfx\test-beep.wav'
$dir  = Split-Path $path -Parent
New-Item -ItemType Directory -Force -Path $dir | Out-Null

$sampleRate = 44100
$durationS  = 0.1
$freqHz     = 440.0
$numSamples = [int]($sampleRate * $durationS)

$samples = New-Object byte[] ($numSamples * 2)
for ($i = 0; $i -lt $numSamples; $i++) {
    $t = $i / [double]$sampleRate
    $v = [int]([math]::Sin(2.0 * [math]::PI * $freqHz * $t) * 16000)
    $samples[$i * 2]     = ($v -band 0xFF)
    $samples[$i * 2 + 1] = (($v -shr 8) -band 0xFF)
}

$fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Create)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([byte[]][char[]]'RIFF')
$bw.Write([int](36 + $samples.Length))
$bw.Write([byte[]][char[]]'WAVE')
$bw.Write([byte[]][char[]]'fmt ')
$bw.Write([int]16)               # subchunk size
$bw.Write([int16]1)              # PCM
$bw.Write([int16]1)              # mono
$bw.Write([int]$sampleRate)      # sample rate
$bw.Write([int]($sampleRate*2))  # byte rate
$bw.Write([int16]2)              # block align
$bw.Write([int16]16)             # bits per sample
$bw.Write([byte[]][char[]]'data')
$bw.Write([int]$samples.Length)
$bw.Write($samples)
$bw.Close()
$fs.Close()
```

Verify the file is ~8.9 KB (a 0.1s 44.1kHz 16-bit mono PCM payload is 8820 bytes + 44 byte header):

```powershell
Get-Item $path | Select-Object Length
```

Expected length ~8864 bytes.

- [ ] **Step 2: Write failing tests in `tests/test_audio_engine.cpp`**

Match the local harness (`CHECK`, `CHECK_NEAR`, single-main runner — same pattern as `tests/test_animation_player.cpp`):

```cpp
#include "audio/AudioEngine.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <cstdlib>
#include <string>

using namespace iron;

namespace {

// Skip device-touching tests when the harness is on a headless CI box.
// IRON_CI=1 is the convention M9 established for the renderer factory test.
bool runningHeadless() {
    const char* env = std::getenv("IRON_CI");
    return env != nullptr && env[0] == '1';
}

const std::string& testWav() {
    static const std::string p = IRON_REPO_ROOT + std::string("/tests/assets/sfx/test-beep.wav");
    return p;
}

}  // namespace

TEST(AudioEngine_NoCrashWithoutInit) {
    AudioEngine a;
    CHECK(a.initialized() == false);
    // Every call must be safe on an uninitialized engine.
    CHECK(a.loadSound(testWav()) == kInvalidSound);
    a.playSoundAt(kInvalidSound, Vec3{0, 0, 0});
    a.playSoundAt(1234, Vec3{0, 0, 0});  // bogus handle, still safe
    a.setListener(Vec3{0, 0, 0}, Vec3{0, 0, -1}, Vec3{0, 1, 0});
    a.shutdown();  // idempotent before init
}

TEST(AudioEngine_InitAndLoadWav) {
    if (runningHeadless()) return;  // CI has no audio device
    AudioEngine a;
    if (!a.init()) return;  // also OK to fail on the local box
    CHECK(a.initialized() == true);
    const SoundHandle h = a.loadSound(testWav());
    CHECK(h != kInvalidSound);
    a.shutdown();
    CHECK(a.initialized() == false);
}

TEST(AudioEngine_LoadMissingFileReturnsInvalid) {
    if (runningHeadless()) return;
    AudioEngine a;
    if (!a.init()) return;
    const SoundHandle h = a.loadSound("does/not/exist.wav");
    CHECK(h == kInvalidSound);
    a.shutdown();
}

TEST(AudioEngine_PlayInvalidHandleIsNoOp) {
    if (runningHeadless()) return;
    AudioEngine a;
    if (!a.init()) return;
    // Should not crash, should not write to a real source.
    a.playSoundAt(kInvalidSound, Vec3{0, 0, 0});
    a.playSoundAt(9999, Vec3{0, 0, 0});
    a.shutdown();
}

TEST(AudioEngine_SetListenerBeforeInit) {
    AudioEngine a;
    // No init — must be safe.
    a.setListener(Vec3{1, 2, 3}, Vec3{0, 0, -1}, Vec3{0, 1, 0});
}
```

- [ ] **Step 3: Register the test in `tests/CMakeLists.txt`**

Find `iron_add_test(test_character_animator ...)` (added in M25) and add a sibling line:

```cmake
iron_add_test(test_audio_engine)
```

Match the surrounding macro signature exactly — read the file first.

- [ ] **Step 4: Implement `engine/audio/AudioEngine.cpp`**

```cpp
#include "audio/AudioEngine.h"

#include "core/Log.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace iron {

namespace {
constexpr std::size_t kSourcePoolSize = 32;
}

struct AudioEngine::Impl {
    ALCdevice*  device  = nullptr;
    ALCcontext* context = nullptr;

    // Pre-allocated source ring. nextSource_ is a round-robin cursor used
    // by playSoundAt to find the next idle (or stealable) source.
    std::array<ALuint, kSourcePoolSize> sources{};
    std::size_t                         nextSource = 0;

    // Loaded buffers, keyed by the SoundHandle we return to the user.
    std::unordered_map<SoundHandle, ALuint> buffers;
    SoundHandle                              nextHandle = 1;

    bool initialized      = false;
    bool warnedStereo     = false;
    bool warnedExhaustion = false;
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init() {
    if (impl_->initialized) return true;

    impl_->device = alcOpenDevice(nullptr);  // default device
    if (!impl_->device) {
        Log::warn("AudioEngine: alcOpenDevice failed; audio disabled");
        return false;
    }
    impl_->context = alcCreateContext(impl_->device, nullptr);
    if (!impl_->context || !alcMakeContextCurrent(impl_->context)) {
        Log::warn("AudioEngine: alcCreateContext / MakeContextCurrent failed");
        if (impl_->context) alcDestroyContext(impl_->context);
        alcCloseDevice(impl_->device);
        impl_->device  = nullptr;
        impl_->context = nullptr;
        return false;
    }

    alGenSources(static_cast<ALsizei>(impl_->sources.size()),
                 impl_->sources.data());
    if (alGetError() != AL_NO_ERROR) {
        Log::warn("AudioEngine: alGenSources failed");
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(impl_->context);
        alcCloseDevice(impl_->device);
        impl_->device  = nullptr;
        impl_->context = nullptr;
        return false;
    }

    impl_->initialized = true;
    return true;
}

void AudioEngine::shutdown() {
    if (!impl_->initialized) return;

    // Stop everything before deletion.
    for (ALuint s : impl_->sources) {
        alSourceStop(s);
    }
    alDeleteSources(static_cast<ALsizei>(impl_->sources.size()),
                    impl_->sources.data());

    for (auto& [h, b] : impl_->buffers) {
        alDeleteBuffers(1, &b);
    }
    impl_->buffers.clear();

    alcMakeContextCurrent(nullptr);
    if (impl_->context) alcDestroyContext(impl_->context);
    if (impl_->device)  alcCloseDevice(impl_->device);
    impl_->context     = nullptr;
    impl_->device      = nullptr;
    impl_->initialized = false;
}

SoundHandle AudioEngine::loadSound(const std::string& wavPath) {
    if (!impl_->initialized) return kInvalidSound;

    unsigned int channels   = 0;
    unsigned int sampleRate = 0;
    drwav_uint64 frameCount = 0;
    drwav_int16* pcm = drwav_open_file_and_read_pcm_frames_s16(
        wavPath.c_str(), &channels, &sampleRate, &frameCount, nullptr);
    if (!pcm || frameCount == 0) {
        Log::warn("AudioEngine: failed to decode WAV '%s'", wavPath.c_str());
        if (pcm) drwav_free(pcm, nullptr);
        return kInvalidSound;
    }

    ALenum format = AL_NONE;
    if (channels == 1)      format = AL_FORMAT_MONO16;
    else if (channels == 2) format = AL_FORMAT_STEREO16;
    else {
        Log::warn("AudioEngine: unsupported channel count %u in '%s'",
                  channels, wavPath.c_str());
        drwav_free(pcm, nullptr);
        return kInvalidSound;
    }

    if (channels == 2 && !impl_->warnedStereo) {
        Log::warn("AudioEngine: '%s' is stereo — OpenAL will play it non-"
                  "positionally. Ship mono for 3D SFX.", wavPath.c_str());
        impl_->warnedStereo = true;
    }

    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, format, pcm,
                 static_cast<ALsizei>(frameCount * channels * sizeof(drwav_int16)),
                 static_cast<ALsizei>(sampleRate));
    drwav_free(pcm, nullptr);

    if (alGetError() != AL_NO_ERROR) {
        Log::warn("AudioEngine: alBufferData failed for '%s'", wavPath.c_str());
        alDeleteBuffers(1, &buffer);
        return kInvalidSound;
    }

    const SoundHandle h = impl_->nextHandle++;
    impl_->buffers[h] = buffer;
    return h;
}

void AudioEngine::playSoundAt(SoundHandle h, Vec3 worldPos, float gain) {
    if (!impl_->initialized || h == kInvalidSound) return;
    auto it = impl_->buffers.find(h);
    if (it == impl_->buffers.end()) return;

    // Find an idle source; if none, voice-steal the next round-robin slot.
    ALuint chosen = 0;
    for (std::size_t i = 0; i < impl_->sources.size(); ++i) {
        const ALuint s = impl_->sources[i];
        ALint state = 0;
        alGetSourcei(s, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) {
            chosen = s;
            break;
        }
    }
    if (chosen == 0) {
        if (!impl_->warnedExhaustion) {
            Log::warn("AudioEngine: source pool exhausted; voice-stealing");
            impl_->warnedExhaustion = true;
        }
        chosen = impl_->sources[impl_->nextSource];
        impl_->nextSource = (impl_->nextSource + 1) % impl_->sources.size();
        alSourceStop(chosen);
    }

    alSourcei (chosen, AL_BUFFER,           static_cast<ALint>(it->second));
    alSource3f(chosen, AL_POSITION,         worldPos.x, worldPos.y, worldPos.z);
    alSource3f(chosen, AL_VELOCITY,         0.0f, 0.0f, 0.0f);
    alSourcef (chosen, AL_GAIN,             gain);
    alSourcei (chosen, AL_SOURCE_RELATIVE,  AL_FALSE);
    alSourcePlay(chosen);
}

void AudioEngine::setListener(Vec3 cameraPos, Vec3 forward, Vec3 up) {
    if (!impl_->initialized) return;
    alListener3f(AL_POSITION, cameraPos.x, cameraPos.y, cameraPos.z);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    const float orient[6] = {forward.x, forward.y, forward.z,
                             up.x,      up.y,      up.z};
    alListenerfv(AL_ORIENTATION, orient);
}

bool AudioEngine::initialized() const {
    return impl_->initialized;
}

}  // namespace iron
```

- [ ] **Step 5: Run tests to verify they pass**

```powershell
cmake --build build-vk --target test_audio_engine --config Debug
ctest --test-dir build-vk -C Debug -R audio_engine --output-on-failure
```

Expected: `test_audio_engine` passes. On a box with no audio device, the init-dependent tests early-return — that's fine.

- [ ] **Step 6: Full test suite — no regressions**

```powershell
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 40 + 1 = 41 tests green (M25 baseline was 40).

- [ ] **Step 7: Commit**

```bash
git add engine/audio/AudioEngine.cpp tests/test_audio_engine.cpp tests/assets/sfx/test-beep.wav tests/CMakeLists.txt
git commit -m "M26 Task 3: AudioEngine implementation + smoke tests"
```

---

## Task 4: Vendor the explosion SFX

**Files:**
- Create: `games/07-net-shooter/assets/sfx/rocket-explode.wav`

- [ ] **Step 1: Pick + download a CC0 explosion sample**

Pick any CC0/public-domain explosion sample, ~1-2s long, mono, 16-bit PCM, 44.1 or 48 kHz. Good sources:

- https://freesound.org/search/?q=explosion&f=license%3A%22Creative+Commons+0%22+duration%3A%5B0+TO+2.0%5D
- https://opengameart.org/art-search-advanced?keys=explosion&field_art_type_tid%5B%5D=13&sort_by=count&sort_order=DESC

Specific candidate the implementer should evaluate first: `https://freesound.org/people/tommccann/sounds/235968/` (CC0, "Explosion 6", ~1.4s mono). If that URL 404s or the licence has changed, pick another CC0 explosion and note the source in the commit message.

Save to `games/07-net-shooter/assets/sfx/rocket-explode.wav`. If the source is OGG or MP3, convert to mono 16-bit PCM WAV first (Audacity / ffmpeg / SoX — implementer's choice).

Verify the file:

```powershell
$p = 'C:\Users\elias\Documents\_dev\iron-core-engine\games\07-net-shooter\assets\sfx\rocket-explode.wav'
Get-Item $p | Select-Object Length
Get-Content $p -TotalCount 1 -Encoding Byte
```

Expected: a few hundred KB. First 4 bytes should be `RIFF` (82, 73, 70, 70 in decimal).

- [ ] **Step 2: Smoke-load via the AudioEngine smoke test**

Optional — add one more test block to `tests/test_audio_engine.cpp` that loads the production WAV too, OR rely on Task 5's visual verification. The simpler path is to skip this and let Task 5 catch any issue.

- [ ] **Step 3: Commit (with source attribution in the message)**

```bash
git add games/07-net-shooter/assets/sfx/rocket-explode.wav
git commit -m "M26 Task 4: vendor rocket-explode.wav (CC0 from freesound.org #235968 by tommccann)"
```

Adjust the message to reflect the actual source used.

---

## Task 5: Wire net-shooter

**Files:**
- Modify: `games/07-net-shooter/main.cpp`

- [ ] **Step 1: Add the include**

Near the top with the other engine includes:

```cpp
#include "audio/AudioEngine.h"
```

- [ ] **Step 2: Construct + init the AudioEngine at startup; load the SFX**

After the renderer + asset-loading block (search for the `litShader` creation or the M25 `foxModel` block — pick the latter as anchor since M26 fits semantically next to other engine resources), add:

```cpp
// --- M26 -------------------------------------------------------------
// Audio engine. Failure to init is non-fatal — the game runs silent.
iron::AudioEngine audio;
if (!audio.init()) {
    iron::Log::warn("net-shooter: AudioEngine init failed; running silent");
}
const iron::SoundHandle boomSfx =
    audio.loadSound("assets/sfx/rocket-explode.wav");
```

- [ ] **Step 3: Update the listener every frame**

Find the render-loop scope (search for the per-frame block where `setUpdate` / `setRender` lambdas are or where `dt` is computed — around line 1413 the frame delta is computed). Find the existing `cameraPos` and look-forward derivation that drives the view matrix (search for `viewMatrix` or `lookAt` or `forwardXZ`).

Right after the view matrix is computed (before any rocket-FX submission, before `submitPlayerFox`), add:

```cpp
// M26 — listener follows the rendering camera. forward/up unit vectors.
audio.setListener(cameraPos,
                  iron::Vec3{lookForward.x, lookForward.y, lookForward.z},
                  iron::Vec3{0.0f, 1.0f, 0.0f});
```

The exact local-variable names depend on what's in scope at the call site. If `cameraPos` doesn't exist, look for the view matrix's translation — the FreeFlyCamera or FirstPersonController will have a position accessor.

- [ ] **Step 4: Play the explosion at all three despawn sites**

Three places fire `DespawnProjectileMsg`:

  1. **CLIENT receive handler** at `main.cpp:1195-1207` — `DespawnProjectileMsg` handler. After `explosions.push_back({...})`, add:

     ```cpp
     audio.playSoundAt(boomSfx,
                       iron::Vec3{msg.x, msg.y, msg.z});
     ```

  2. **HOST hit broadcast** at `main.cpp:1864-1869` — after the existing `explosions.push_back(ExplosionFx{d.point, simNow});`, add:

     ```cpp
     audio.playSoundAt(boomSfx, d.point);
     ```

  3. **HOST lifetime-cap broadcast** at `main.cpp:1885-1890` — after the existing `explosions.push_back(ExplosionFx{pos, simNow});` in the lifetime-cap loop, add:

     ```cpp
     audio.playSoundAt(boomSfx, pos);
     ```

All three are inside `if (peers.isHost())` contexts (or the client-only handler), so they don't double-fire.

- [ ] **Step 5: Build clean**

```powershell
cmake --build build-vk --target net-shooter --config Debug
```

Expected: clean build, no warnings. Watch out for unused-variable warnings on `boomSfx` if any branch was missed.

- [ ] **Step 6: Commit**

```bash
git add games/07-net-shooter/main.cpp
git commit -m "M26 Task 5: play rocket-explode SFX at all detonation sites"
```

---

## Task 6: Docs + PR

**Files:**
- Modify: `docs/engine/asset-pipeline.md`

- [ ] **Step 1: Append an M26 section**

Match the style of M23/M24/M25 sections. Cover:

- **What got added:** `iron::AudioEngine` + `SoundHandle`; OpenAL Soft (vcpkg) + dr_wav (vendored). Net-shooter integration plays one positional 3D SFX at rocket detonation.
- **Engine API** (the public surface from `AudioEngine.h`).
- **Runtime data flow:** WAV file → `loadSound` → OpenAL buffer → `playSoundAt(handle, worldPos)` → source ring → OpenAL mixer → speakers. Listener updated per frame from camera state.
- **Asset pipeline note:** WAV only in v1; mono recommended for 3D positioning (stereo plays non-positionally per OpenAL spec).
- **Limitations / non-goals:** no looping, no doppler, no buses, no compressed formats — all deferred to M27+.
- **Verification:**
  ```powershell
  .\build-vk\games\07-net-shooter\Debug\net-shooter.exe --host
  # second terminal:
  .\build-vk\games\07-net-shooter\Debug\net-shooter.exe --client 127.0.0.1
  ```
  Expected: rockets fired by either peer produce a positional boom on both screens.

- [ ] **Step 2: Commit, push, open PR**

```bash
git add docs/engine/asset-pipeline.md
git commit -m "M26: document AudioEngine in asset-pipeline.md"
git push -u origin feat/m26-audio-foundation
```

Open the PR matching the M25 PR (#45) template:

```powershell
gh pr create --title "M26: Audio foundation (OpenAL Soft + WAV)" --body "$(cat <<'EOF'
## Summary

- Added `iron::AudioEngine` engine type — opens OpenAL Soft device, manages a 32-source pool, decodes WAVs via vendored dr_wav (public domain), exposes `loadSound` / `playSoundAt` / `setListener`.
- Added OpenAL Soft to vcpkg manifest; vendored dr_wav.h under `third_party/dr_libs/`.
- Wired net-shooter to play one positional 3D explosion sample (CC0) at every rocket detonation site — both host and client paths. Listener follows the rendering camera.
- Zero new network bandwidth; explosions piggy-back on the existing `DespawnProjectileMsg`.

## Test plan

- [x] Unit tests pass locally (AudioEngine init/load/play/no-init safety — 41/41)
- [x] net-shooter builds clean
- [ ] Visual verification (host + client): rockets produce positional booms; sound pans with camera turn

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Verification Checklist (before declaring done)

- [ ] `ctest --test-dir build-vk -C Debug --output-on-failure` — full suite green (40 → 41).
- [ ] `net-shooter` builds clean, no warnings.
- [ ] Visual: host + client both running. Fire a rocket. Both peers hear a positional boom at impact. Rotate the camera before firing — sound direction tracks the rotation.
- [ ] Fire 10 rockets in quick succession; no crash, no source exhaustion crashes (might warn-once).
- [ ] PR CI green.
