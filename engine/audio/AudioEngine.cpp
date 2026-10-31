#include "audio/AudioEngine.h"

#include "core/Log.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <type_traits>
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

    // Looping voices: a reserved-source set + VoiceId -> pool-slot map.
    std::array<bool, kSourcePoolSize>      reserved{};
    std::unordered_map<VoiceId, std::size_t> voiceSlot;
    VoiceId                                  nextVoice = 1;

    // Effects are an extension, so their functions are looked up at runtime.
    LPALGENEFFECTS                effectsGen = nullptr;
    LPALDELETEEFFECTS             effectsDelete = nullptr;
    LPALEFFECTI                   effectI = nullptr;
    LPALEFFECTF                   effectF = nullptr;
    LPALGENAUXILIARYEFFECTSLOTS   slotsGen = nullptr;
    LPALDELETEAUXILIARYEFFECTSLOTS slotsDelete = nullptr;
    LPALAUXILIARYEFFECTSLOTI      slotI = nullptr;
    LPALGENFILTERS                filtersGen = nullptr;
    LPALDELETEFILTERS             filtersDelete = nullptr;
    LPALFILTERI                   filterI = nullptr;
    LPALFILTERF                   filterF = nullptr;
    ALuint                        reverbEffect = 0;
    ALuint                        reverbSlot = 0;
    std::array<ALuint, kSourcePoolSize> muffles{};  // One lowpass per source.
    bool                          effects = false;

    bool initialized      = false;
    bool warnedStereo     = false;
    bool warnedExhaustion = false;
};

namespace {

// Sends every source to one reverb, and gives each a filter of its own.
template <typename Impl>
bool initEffects(Impl& impl) {
    if (!alcIsExtensionPresent(impl.device, ALC_EXT_EFX_NAME)) return false;

    const auto find = [](auto& function, const char* name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(alGetProcAddress(name));
        return function != nullptr;
    };
    if (!find(impl.effectsGen, "alGenEffects") || !find(impl.effectsDelete, "alDeleteEffects") ||
        !find(impl.effectI, "alEffecti") || !find(impl.effectF, "alEffectf") ||
        !find(impl.slotsGen, "alGenAuxiliaryEffectSlots") ||
        !find(impl.slotsDelete, "alDeleteAuxiliaryEffectSlots") ||
        !find(impl.slotI, "alAuxiliaryEffectSloti") || !find(impl.filtersGen, "alGenFilters") ||
        !find(impl.filtersDelete, "alDeleteFilters") || !find(impl.filterI, "alFilteri") ||
        !find(impl.filterF, "alFilterf")) {
        return false;
    }

    alGetError();
    impl.effectsGen(1, &impl.reverbEffect);
    impl.effectI(impl.reverbEffect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
    impl.slotsGen(1, &impl.reverbSlot);
    impl.slotI(impl.reverbSlot, AL_EFFECTSLOT_EFFECT, static_cast<ALint>(impl.reverbEffect));

    impl.filtersGen(static_cast<ALsizei>(impl.muffles.size()), impl.muffles.data());
    for (std::size_t i = 0; i < impl.sources.size(); ++i) {
        impl.filterI(impl.muffles[i], AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        impl.filterF(impl.muffles[i], AL_LOWPASS_GAIN, 1.0f);
        impl.filterF(impl.muffles[i], AL_LOWPASS_GAINHF, 1.0f);
        alSource3i(impl.sources[i], AL_AUXILIARY_SEND_FILTER, static_cast<ALint>(impl.reverbSlot), 0, AL_FILTER_NULL);
    }
    return alGetError() == AL_NO_ERROR;
}

}  // namespace

// Takes ownership of `pcm`. Returns 0 on failure.
template <typename Impl>
ALuint bufferFromPcm(Impl& impl, drwav_int16* pcm, unsigned int channels, unsigned int sampleRate,
                     drwav_uint64 frameCount, const std::string& wavPath);

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
    const ALCint attributes[] = {ALC_MAX_AUXILIARY_SENDS, 1, 0};
    impl_->context = alcCreateContext(impl_->device, attributes);
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
    impl_->effects = initEffects(*impl_);
    if (!impl_->effects) Log::warn("AudioEngine: effects unavailable; playing without reverb");
    return true;
}

void AudioEngine::shutdown() {
    if (!impl_->initialized) return;

    // Stop everything before deletion.
    for (ALuint s : impl_->sources) {
        alSourceStop(s);
        if (impl_->effects) {
            alSource3i(s, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
            alSourcei(s, AL_DIRECT_FILTER, AL_FILTER_NULL);
        }
    }
    if (impl_->effects) {
        impl_->slotI(impl_->reverbSlot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
        impl_->slotsDelete(1, &impl_->reverbSlot);
        impl_->effectsDelete(1, &impl_->reverbEffect);
        impl_->filtersDelete(static_cast<ALsizei>(impl_->muffles.size()), impl_->muffles.data());
        impl_->effects = false;
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
    const ALuint buffer = bufferFromPcm(*impl_, pcm, channels, sampleRate, frameCount, wavPath);
    if (buffer == 0) return kInvalidSound;

    const SoundHandle h = impl_->nextHandle++;
    impl_->buffers[h] = Impl::LoadedSound{buffer, wavPath, fileMtime(wavPath)};
    return h;
}

SoundHandle AudioEngine::loadSoundFromMemory(const void* wav, std::size_t size, const std::string& label) {
    if (!impl_->initialized) return kInvalidSound;

    unsigned int channels   = 0;
    unsigned int sampleRate = 0;
    drwav_uint64 frameCount = 0;
    drwav_int16* pcm = drwav_open_memory_and_read_pcm_frames_s16(
        wav, size, &channels, &sampleRate, &frameCount, nullptr);
    const ALuint buffer = bufferFromPcm(*impl_, pcm, channels, sampleRate, frameCount, label);
    if (buffer == 0) return kInvalidSound;

    const SoundHandle h = impl_->nextHandle++;
    impl_->buffers[h] = Impl::LoadedSound{buffer, std::string{}, 0};
    return h;
}

// Takes ownership of `pcm`. Returns 0 on failure.
template <typename Impl>
ALuint bufferFromPcm(Impl& impl, drwav_int16* pcm, unsigned int channels, unsigned int sampleRate,
                     drwav_uint64 frameCount, const std::string& wavPath) {
    if (!pcm || frameCount == 0) {
        Log::warn("AudioEngine: failed to decode WAV '%s'", wavPath.c_str());
        if (pcm) drwav_free(pcm, nullptr);
        return 0;
    }

    ALenum format = AL_NONE;
    if (channels == 1)      format = AL_FORMAT_MONO16;
    else if (channels == 2) format = AL_FORMAT_STEREO16;
    else {
        Log::warn("AudioEngine: unsupported channel count %u in '%s'",
                  channels, wavPath.c_str());
        drwav_free(pcm, nullptr);
        return 0;
    }

    if (channels == 2 && !impl.warnedStereo) {
        Log::warn("AudioEngine: '%s' is stereo - OpenAL will play it non-"
                  "positionally. Ship mono for 3D SFX.", wavPath.c_str());
        impl.warnedStereo = true;
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
        return 0;
    }
    return buffer;
}

void AudioEngine::playSoundAt(SoundHandle h, Vec3 worldPos, float gain, float pitch) {
    if (!impl_->initialized || h == kInvalidSound) return;
    auto it = impl_->buffers.find(h);
    if (it == impl_->buffers.end()) return;

    // Find an idle source; if none, voice-steal the next round-robin slot.
    ALuint chosen = 0;
    for (std::size_t i = 0; i < impl_->sources.size(); ++i) {
        if (impl_->reserved[i]) continue;
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
    }

    alSourcei (chosen, AL_BUFFER,           static_cast<ALint>(it->second.buffer));
    alSource3f(chosen, AL_POSITION,         worldPos.x, worldPos.y, worldPos.z);
    alSource3f(chosen, AL_VELOCITY,         0.0f, 0.0f, 0.0f);
    alSourcef (chosen, AL_GAIN,             gain);
    alSourcef (chosen, AL_PITCH,            pitch);
    alSourcei (chosen, AL_SOURCE_RELATIVE,  AL_FALSE);
    alSourcePlay(chosen);
}

void AudioEngine::playSoundLocal(SoundHandle h, float gain, float pitch) {
    if (!impl_->initialized || h == kInvalidSound) return;
    auto it = impl_->buffers.find(h);
    if (it == impl_->buffers.end()) return;

    // Find an idle source; voice-steal round-robin on exhaustion (same as
    // playSoundAt — the underlying source-pool logic is shared).
    ALuint chosen = 0;
    for (std::size_t i = 0; i < impl_->sources.size(); ++i) {
        if (impl_->reserved[i]) continue;
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
    }

    alSourcei (chosen, AL_BUFFER,           static_cast<ALint>(it->second.buffer));
    alSource3f(chosen, AL_POSITION,         0.0f, 0.0f, 0.0f);
    alSource3f(chosen, AL_VELOCITY,         0.0f, 0.0f, 0.0f);
    alSourcef (chosen, AL_GAIN,             gain);
    alSourcef (chosen, AL_PITCH,            pitch);
    // AL_SOURCE_RELATIVE=TRUE means the source position is interpreted in
    // listener-local space; position (0,0,0) = "right at the listener", so
    // OpenAL skips spatialization and plays the sound centered.
    alSourcei (chosen, AL_SOURCE_RELATIVE,  AL_TRUE);
    alSourcePlay(chosen);
}

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
    alSourcef(src, AL_PITCH,   1.0f);  // Sources are reused, so pitch mustn't carry over.
    impl_->reserved[slot] = false;
    impl_->voiceSlot.erase(it);
}

void AudioEngine::setVoicePosition(VoiceId v, Vec3 worldPos) {
    if (!impl_->initialized || v == kInvalidVoice) return;
    auto it = impl_->voiceSlot.find(v);
    if (it == impl_->voiceSlot.end()) return;
    alSource3f(impl_->sources[it->second], AL_POSITION, worldPos.x, worldPos.y, worldPos.z);
}

void AudioEngine::setVoiceGain(VoiceId v, float gain) {
    if (!impl_->initialized || v == kInvalidVoice) return;
    auto it = impl_->voiceSlot.find(v);
    if (it == impl_->voiceSlot.end()) return;
    alSourcef(impl_->sources[it->second], AL_GAIN, gain);
}

void AudioEngine::setVoicePitch(VoiceId v, float pitch) {
    if (!impl_->initialized || v == kInvalidVoice) return;
    auto it = impl_->voiceSlot.find(v);
    if (it == impl_->voiceSlot.end()) return;
    alSourcef(impl_->sources[it->second], AL_PITCH, pitch);
}

void AudioEngine::setVoiceMuffle(VoiceId v, float highs) {
    if (!impl_->initialized || !impl_->effects || v == kInvalidVoice) return;
    auto it = impl_->voiceSlot.find(v);
    if (it == impl_->voiceSlot.end()) return;
    const std::size_t slot = it->second;
    impl_->filterF(impl_->muffles[slot], AL_LOWPASS_GAINHF, std::clamp(highs, 0.0f, 1.0f));
    alSourcei(impl_->sources[slot], AL_DIRECT_FILTER, static_cast<ALint>(impl_->muffles[slot]));
}

void AudioEngine::setReverb(const ReverbSettings& s) {
    if (!impl_->initialized || !impl_->effects) return;
    const ALuint e = impl_->reverbEffect;
    impl_->effectF(e, AL_REVERB_DENSITY, std::clamp(s.density, 0.0f, 1.0f));
    impl_->effectF(e, AL_REVERB_DIFFUSION, std::clamp(s.diffusion, 0.0f, 1.0f));
    impl_->effectF(e, AL_REVERB_GAIN, std::clamp(s.gain, 0.0f, 1.0f));
    impl_->effectF(e, AL_REVERB_GAINHF, std::clamp(s.gainHF, 0.0f, 1.0f));
    impl_->effectF(e, AL_REVERB_DECAY_TIME, std::clamp(s.decaySeconds, 0.1f, 20.0f));
    impl_->effectF(e, AL_REVERB_REFLECTIONS_GAIN, std::clamp(s.reflectionsGain, 0.0f, 3.16f));
    impl_->effectF(e, AL_REVERB_REFLECTIONS_DELAY, std::clamp(s.reflectionsDelay, 0.0f, 0.3f));
    impl_->effectF(e, AL_REVERB_LATE_REVERB_GAIN, std::clamp(s.lateGain, 0.0f, 10.0f));
    impl_->effectF(e, AL_REVERB_LATE_REVERB_DELAY, std::clamp(s.lateDelay, 0.0f, 0.1f));
    // A slot keeps its own copy of an effect, so a change only lands on re-attaching.
    impl_->slotI(impl_->reverbSlot, AL_EFFECTSLOT_EFFECT, static_cast<ALint>(e));
}

void AudioEngine::setMasterGain(float gain) {
    if (!impl_->initialized) return;
    alListenerf(AL_GAIN, std::clamp(gain, 0.0f, 1.0f));
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
