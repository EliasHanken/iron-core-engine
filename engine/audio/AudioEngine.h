#pragma once

#include "math/Vec.h"

#include <cstdint>
#include <memory>
#include <string>

namespace iron {

using SoundHandle = std::uint32_t;
constexpr SoundHandle kInvalidSound = 0;

using VoiceId = std::uint32_t;
constexpr VoiceId kInvalidVoice = 0;

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

    // Play `h` non-positionally (always centered at the listener with full
    // stereo balance, regardless of listener pose). Use for first-person
    // "ego sounds" — the local player's own gunshot, footsteps, jump, etc.
    // — where 3D spatialization causes panning artifacts as the camera moves.
    // No-op if `h == kInvalidSound` or the engine failed to init.
    void playSoundLocal(SoundHandle h, float gain = 1.0f);

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

    // Update the listener (camera) state. Call once per frame before any
    // playSoundAt calls. `forward` and `up` are unit vectors describing
    // the listener's orientation; match the rendering camera.
    void setListener(Vec3 cameraPos, Vec3 forward, Vec3 up);

    // M28 — re-decode any loaded sound whose source WAV changed on disk
    // since load, swapping the OpenAL buffer in place (the SoundHandle
    // stays valid). Call once per frame. No-op if the engine failed to
    // init. Polls each loaded sound's file mtime internally — no external
    // watcher needed.
    void pollHotReload();

    bool initialized() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iron
