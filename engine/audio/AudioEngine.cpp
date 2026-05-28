#include "audio/AudioEngine.h"

#include "core/Log.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace iron {

namespace {
constexpr std::size_t kSourcePoolSize = 32;

// File mtime as a stable integer (ticks since the clock epoch). Returns 0
// if the file is missing or unreadable.
std::int64_t fileMtime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}
}

struct AudioEngine::Impl {
    ALCdevice*  device  = nullptr;
    ALCcontext* context = nullptr;

    // Pre-allocated source ring. nextSource is a round-robin cursor used
    // by playSoundAt to find the next idle (or stealable) source.
    std::array<ALuint, kSourcePoolSize> sources{};
    std::size_t                         nextSource = 0;

    // Loaded buffers, keyed by the SoundHandle we return to the user. Each
    // record carries the source path + mtime so pollHotReload can re-decode
    // a WAV that changed on disk and swap the buffer in place.
    struct LoadedSound {
        ALuint        buffer = 0;
        std::string   path;
        std::int64_t  mtime = 0;   // last_write_time ticks at load
    };
    std::unordered_map<SoundHandle, LoadedSound> buffers;
    SoundHandle                                  nextHandle = 1;

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

    for (auto& [h, snd] : impl_->buffers) {
        alDeleteBuffers(1, &snd.buffer);
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
        Log::warn("AudioEngine: '%s' is stereo - OpenAL will play it non-"
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
    impl_->buffers[h] = Impl::LoadedSound{buffer, wavPath, fileMtime(wavPath)};
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

    alSourcei (chosen, AL_BUFFER,           static_cast<ALint>(it->second.buffer));
    alSource3f(chosen, AL_POSITION,         worldPos.x, worldPos.y, worldPos.z);
    alSource3f(chosen, AL_VELOCITY,         0.0f, 0.0f, 0.0f);
    alSourcef (chosen, AL_GAIN,             gain);
    alSourcei (chosen, AL_SOURCE_RELATIVE,  AL_FALSE);
    alSourcePlay(chosen);
}

void AudioEngine::playSoundLocal(SoundHandle h, float gain) {
    if (!impl_->initialized || h == kInvalidSound) return;
    auto it = impl_->buffers.find(h);
    if (it == impl_->buffers.end()) return;

    // Find an idle source; voice-steal round-robin on exhaustion (same as
    // playSoundAt — the underlying source-pool logic is shared).
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

    alSourcei (chosen, AL_BUFFER,           static_cast<ALint>(it->second.buffer));
    alSource3f(chosen, AL_POSITION,         0.0f, 0.0f, 0.0f);
    alSource3f(chosen, AL_VELOCITY,         0.0f, 0.0f, 0.0f);
    alSourcef (chosen, AL_GAIN,             gain);
    // AL_SOURCE_RELATIVE=TRUE means the source position is interpreted in
    // listener-local space; position (0,0,0) = "right at the listener", so
    // OpenAL skips spatialization and plays the sound centered.
    alSourcei (chosen, AL_SOURCE_RELATIVE,  AL_TRUE);
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

void AudioEngine::pollHotReload() {
    if (!impl_->initialized) return;

    for (auto& [h, snd] : impl_->buffers) {
        if (snd.path.empty()) continue;
        const std::int64_t now = fileMtime(snd.path);
        if (now == 0 || now == snd.mtime) continue;  // missing / unchanged

        unsigned int channels   = 0;
        unsigned int sampleRate = 0;
        drwav_uint64 frameCount = 0;
        drwav_int16* pcm = drwav_open_file_and_read_pcm_frames_s16(
            snd.path.c_str(), &channels, &sampleRate, &frameCount, nullptr);
        if (!pcm || frameCount == 0) {
            // Likely a mid-write read; leave mtime unchanged and retry later.
            if (pcm) drwav_free(pcm, nullptr);
            continue;
        }
        ALenum format = (channels == 1) ? AL_FORMAT_MONO16
                      : (channels == 2) ? AL_FORMAT_STEREO16 : AL_NONE;
        if (format == AL_NONE) {
            drwav_free(pcm, nullptr);
            snd.mtime = now;  // unsupported format — don't retry every frame
            Log::warn("AudioEngine: hot-reload skipped '%s' (unsupported channels %u)",
                      snd.path.c_str(), channels);
            continue;
        }

        // Stop any source currently playing the old buffer so it's safe to
        // delete. A clipped tail on one in-flight sound is acceptable.
        for (ALuint src : impl_->sources) {
            ALint buf = 0;
            alGetSourcei(src, AL_BUFFER, &buf);
            if (static_cast<ALuint>(buf) == snd.buffer) {
                alSourceStop(src);
                alSourcei(src, AL_BUFFER, 0);
            }
        }

        ALuint newBuffer = 0;
        alGenBuffers(1, &newBuffer);
        alBufferData(newBuffer, format, pcm,
                     static_cast<ALsizei>(frameCount * channels * sizeof(drwav_int16)),
                     static_cast<ALsizei>(sampleRate));
        drwav_free(pcm, nullptr);
        if (alGetError() != AL_NO_ERROR) {
            alDeleteBuffers(1, &newBuffer);
            continue;  // keep old buffer + mtime; retry next poll
        }

        alDeleteBuffers(1, &snd.buffer);
        snd.buffer = newBuffer;
        snd.mtime  = now;
        Log::info("AudioEngine: hot-reloaded '%s'", snd.path.c_str());
    }
}

bool AudioEngine::initialized() const {
    return impl_->initialized;
}

}  // namespace iron
