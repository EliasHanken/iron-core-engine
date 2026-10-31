#pragma once

// Everything Kindle plays, and how it's mixed. Levels glide rather than jump, each
// space has its own acoustics, ignites duck everything else so that they hit, and
// no one-shot plays twice at the same pitch. A log of what played gives a recording
// the same mix afterwards, because the capture path has no audio of its own.

#include "Swarm.h"

#include "audio/AudioEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace kindle {

class Sound {
public:
    ~Sound() {
        if (log_) std::fclose(log_);
    }

    // With `playAloud` false nothing is heard, and only the log is written.
    void init(const std::string& folder, bool playAloud, const std::string& logPath) {
        if (!logPath.empty()) log_ = std::fopen(logPath.c_str(), "w");
        aloud_ = playAloud && audio_.init();
        if (!aloud_) return;

        const auto load = [&](const std::string& name) { return audio_.loadSound(folder + name + ".wav"); };
        ignite_ = load("ignite");
        wake_ = load("wake");
        gate_ = load("gate");
        groan_ = load("groan");
        spool_ = load("spool");
        finale_ = load("finale");
        creaks_ = {load("creak1"), load("creak2"), load("creak3"), load("chain"), load("bend")};
        whooshes_ = {load("whoosh1"), load("whoosh2"), load("whoosh3"), load("whoosh4")};

        room_.voice = audio_.playLooping(load("room"), Vec3{}, 0.0f);
        roar_.voice = audio_.playLooping(load("roar"), Vec3{}, 0.0f);
        crackle_.voice = audio_.playLooping(load("crackle"), Vec3{}, 0.0f);
        rush_.voice = audio_.playLooping(load("rush"), Vec3{}, 0.0f);
    }

    // Back to the start of the game, with the room still playing.
    void restart() {
        gateHeard_ = false;
        spoolHeard_ = false;
        finaleHeard_ = false;
        creakIn_ = 3.0f;
    }

    void setVolume(float volume) {
        if (aloud_) audio_.setMasterGain(volume * volume);  // Hearing isn't linear.
    }

    // Paused, the world carries on behind a closed door.
    void setPaused(bool paused) { muffled_ = paused; }

    void menuMove() { play(creaks_[3], "chain", 0.14f, 1.7f); }
    void menuConfirm() { play(wake_, "wake", 0.35f, 1.3f); }

    void update(float seconds, float dt, Vec3 eye, Vec3 forward, const Room& room, const Swarm& swarm,
                const Story& story, bool freshWake, float presence) {
        time_ = seconds;
        const float speed = std::min(swarm.speed() / Swarm::kSqueezeSpeed, 1.0f);
        const float squeeze = swarm.squeeze();
        const float blown = story.sinceIgnition();

        // Ignites duck the beds, so that the hit has room, then they swell back.
        if (swarm.ignited()) duck_ = 1.0f;
        duck_ *= std::exp(-dt * 2.6f);
        const float ducked = 1.0f - 0.65f * duck_;

        const float open = muffled_ ? 0.1f : 1.0f;
        const float behind = muffled_ ? 0.45f : 1.0f;
        glide(room_, "room", (0.55f - (blown > 0.0f ? 0.2f : 0.0f)) * behind * (0.6f + 0.4f * ducked), 1.0f, open, dt);
        glide(roar_, "roar", presence * (0.16f + 0.5f * speed + 0.35f * swarm.flare()) * ducked * behind,
              0.75f + 0.55f * speed + 0.2f * squeeze, open, dt);
        glide(crackle_, "crackle", std::max(presence, 0.14f) * (0.3f + 0.4f * speed) * ducked * behind, 1.0f, open, dt);
        glide(rush_, "rush", presence * (squeeze * 0.6f + speed * 0.1f) * ducked * behind, 0.9f + 0.4f * squeeze,
              open, dt);

        if (aloud_) {
            audio_.setListener(eye, forward, {0, 1, 0});
            const Vec3 close = eye + forward;
            audio_.setVoicePosition(roar_.voice, close);
            audio_.setVoicePosition(crackle_.voice, close);
            audio_.setVoicePosition(rush_.voice, close);
            updateAcoustics(swarm.position(), dt);
        }

        if (muffled_) return;

        if (swarm.ignited()) play(ignite_, "ignite", 0.9f, varied());
        if (freshWake) play(wake_, "wake", 0.7f, varied());

        if (story.gate() > 0.0f && !gateHeard_) {
            gateHeard_ = true;
            play(gate_, "gate", 1.0f, 1.0f);
            play(groan_, "groan", 0.5f, 1.0f);
        }
        if (story.pulling() && !spoolHeard_) {
            spoolHeard_ = true;
            play(spool_, "spool", 0.9f, 1.0f);
        }
        if (blown > 0.0f && !finaleHeard_) {
            finaleHeard_ = true;
            play(finale_, "finale", 1.0f, 0.85f);
            play(wake_, "wake", 1.0f, 0.7f);
        }

        // Skimming past a surface at speed cuts the air.
        whooshIn_ -= dt;
        const float clearance = roomDistance(room, swarm.position());
        if (whooshIn_ <= 0.0f && speed > 0.42f && clearance < 2.4f) {
            whooshIn_ = 0.55f;
            const std::size_t which = differentFrom(lastWhoosh_, whooshes_.size());
            lastWhoosh_ = which;
            play(whooshes_[which], kWhooshNames[which], 0.3f + 0.45f * speed, 0.85f + 0.4f * speed);
        }

        // The machine shifts in its sleep, somewhere off in the dark.
        creakIn_ -= dt;
        if (creakIn_ <= 0.0f) {
            creakIn_ = random_.range(5.0f, 13.0f);
            const std::size_t which = differentFrom(lastCreak_, creaks_.size());
            lastCreak_ = which;
            const Vec3 off{random_.range(-18.0f, 18.0f), random_.range(-4.0f, 5.0f), random_.range(-18.0f, 18.0f)};
            playFar(which, eye + off);
        }
    }

private:
    static constexpr std::array<const char*, 5> kCreakNames{"creak1", "creak2", "creak3", "chain", "bend"};
    static constexpr std::array<const char*, 4> kWhooshNames{"whoosh1", "whoosh2", "whoosh3", "whoosh4"};

    class Random {
    public:
        float range(float low, float high) {
            state_ = state_ * 1664525u + 1013904223u;
            return low + (high - low) * (static_cast<float>(state_ >> 8) / 16777216.0f);
        }

    private:
        std::uint32_t state_ = 977u;
    };

    struct Bed {
        iron::VoiceId voice = iron::kInvalidVoice;
        float gain = 0.0f;
        float pitch = 1.0f;
        float highs = 1.0f;
    };

    float varied() { return random_.range(0.93f, 1.07f); }

    std::size_t differentFrom(std::size_t last, std::size_t count) {
        const auto pick = static_cast<std::size_t>(random_.range(0.0f, static_cast<float>(count) - 1.01f));
        return pick >= last ? pick + 1 : pick;
    }

    void glide(Bed& bed, const char* name, float gain, float pitch, float highs, float dt) {
        const float k = 1.0f - std::exp(-dt * 9.0f);
        bed.gain += (gain - bed.gain) * k;
        bed.pitch += (pitch - bed.pitch) * k;
        bed.highs += (highs - bed.highs) * k;
        if (aloud_) {
            audio_.setVoiceGain(bed.voice, bed.gain);
            audio_.setVoicePitch(bed.voice, bed.pitch);
            audio_.setVoiceMuffle(bed.voice, bed.highs);
        }
        if (log_) std::fprintf(log_, "%.4f,bed,%s,%.4f,%.4f\n", time_, name, bed.gain, bed.pitch);
    }

    // The room you're in sets the reverb: a long hall, a tight slit and a vast chamber.
    void updateAcoustics(Vec3 position, float dt) {
        static constexpr float kHallEnd = -19.0f;
        static constexpr float kSlitEnd = -29.0f;
        iron::ReverbSettings hall;
        hall.decaySeconds = 3.4f;
        hall.gain = 0.42f;
        hall.gainHF = 0.5f;
        hall.reflectionsGain = 0.12f;
        hall.reflectionsDelay = 0.03f;
        hall.lateGain = 1.1f;
        hall.lateDelay = 0.03f;
        iron::ReverbSettings slit;
        slit.decaySeconds = 1.0f;
        slit.gain = 0.5f;
        slit.gainHF = 0.7f;
        slit.reflectionsGain = 0.45f;
        slit.reflectionsDelay = 0.008f;
        slit.lateGain = 0.9f;
        slit.lateDelay = 0.012f;
        iron::ReverbSettings chamber;
        chamber.decaySeconds = 7.5f;
        chamber.gain = 0.6f;
        chamber.gainHF = 0.4f;
        chamber.reflectionsGain = 0.05f;
        chamber.reflectionsDelay = 0.09f;
        chamber.lateGain = 1.4f;
        chamber.lateDelay = 0.05f;

        const float z = position.z;
        const iron::ReverbSettings& target = z > kHallEnd ? hall : (z > kSlitEnd ? slit : chamber);
        const float k = 1.0f - std::exp(-dt * 2.5f);
        const auto ease = [&](float& value, float want) { value += (want - value) * k; };
        ease(reverb_.decaySeconds, target.decaySeconds);
        ease(reverb_.gain, target.gain);
        ease(reverb_.gainHF, target.gainHF);
        ease(reverb_.reflectionsGain, target.reflectionsGain);
        ease(reverb_.reflectionsDelay, target.reflectionsDelay);
        ease(reverb_.lateGain, target.lateGain);
        ease(reverb_.lateDelay, target.lateDelay);

        // Changing a reverb is heavier than a gain, so it's paced.
        reverbIn_ -= dt;
        if (reverbIn_ <= 0.0f) {
            reverbIn_ = 0.1f;
            audio_.setReverb(reverb_);
        }
    }

    void play(iron::SoundHandle sound, const char* name, float gain, float pitch) {
        if (aloud_) audio_.playSoundLocal(sound, gain, pitch);
        if (log_) std::fprintf(log_, "%.4f,shot,%s,%.4f,%.4f\n", time_, name, gain, pitch);
    }

    void playFar(std::size_t which, Vec3 position) {
        const float pitch = varied();
        // Mono voices fall off with distance, so this is loud at the source.
        if (aloud_) audio_.playSoundAt(creaks_[which], position, 7.0f, pitch);
        if (log_) std::fprintf(log_, "%.4f,shot,%s,0.35,%.4f\n", time_, kCreakNames[which], pitch);
    }

    iron::AudioEngine audio_;
    bool aloud_ = false;
    std::FILE* log_ = nullptr;
    float time_ = 0.0f;

    iron::SoundHandle ignite_ = iron::kInvalidSound;
    iron::SoundHandle wake_ = iron::kInvalidSound;
    iron::SoundHandle gate_ = iron::kInvalidSound;
    iron::SoundHandle groan_ = iron::kInvalidSound;
    iron::SoundHandle spool_ = iron::kInvalidSound;
    iron::SoundHandle finale_ = iron::kInvalidSound;
    std::array<iron::SoundHandle, 5> creaks_{};
    std::array<iron::SoundHandle, 4> whooshes_{};

    Bed room_;
    Bed roar_;
    Bed crackle_;
    Bed rush_;

    iron::ReverbSettings reverb_;
    float reverbIn_ = 0.0f;
    float duck_ = 0.0f;
    bool muffled_ = false;

    bool gateHeard_ = false;
    bool spoolHeard_ = false;
    bool finaleHeard_ = false;
    float creakIn_ = 3.0f;
    float whooshIn_ = 0.0f;
    std::size_t lastCreak_ = 0;
    std::size_t lastWhoosh_ = 0;
    Random random_;
};

}  // namespace kindle
