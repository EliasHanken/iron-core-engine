#pragma once

// Everything Kindle plays. Fire and air follow the swarm, and the machine creaks
// around you at random. A log of what played lets a recording get the same mix
// afterwards, because the capture path has no audio of its own.

#include "Swarm.h"

#include "audio/AudioEngine.h"

#include <algorithm>
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

        const auto load = [&](const char* name) { return audio_.loadSound(folder + name + ".wav"); };
        ignite_ = load("ignite");
        wake_ = load("wake");
        gate_ = load("gate");
        groan_ = load("groan");
        spool_ = load("spool");
        finale_ = load("finale");
        creaks_[0] = load("creak1");
        creaks_[1] = load("creak2");
        creaks_[2] = load("creak3");
        creaks_[3] = load("chain");
        creaks_[4] = load("bend");

        room_ = audio_.playLooping(load("room"), Vec3{}, 0.0f);
        roar_ = audio_.playLooping(load("roar"), Vec3{}, 0.0f);
        crackle_ = audio_.playLooping(load("crackle"), Vec3{}, 0.0f);
        rush_ = audio_.playLooping(load("rush"), Vec3{}, 0.0f);
    }

    void update(float seconds, float dt, Vec3 eye, Vec3 forward, const Swarm& swarm, const Story& story,
                bool freshWake, float presence) {
        time_ = seconds;
        const float speed = std::min(swarm.speed() / Swarm::kSqueezeSpeed, 1.0f);
        const float squeeze = swarm.squeeze();
        const float blown = story.sinceIgnition();

        const Bed room{0.55f - (blown > 0.0f ? 0.2f : 0.0f), 1.0f};
        const Bed roar{presence * (0.16f + 0.5f * speed + 0.35f * swarm.flare()),
                       0.75f + 0.55f * speed + 0.2f * squeeze};
        const Bed crackle{std::max(presence, 0.14f) * (0.3f + 0.4f * speed), 1.0f};
        const Bed rush{presence * (squeeze * 0.6f + speed * 0.1f), 0.9f + 0.4f * squeeze};
        setBed(room_, "room", room);
        setBed(roar_, "roar", roar);
        setBed(crackle_, "crackle", crackle);
        setBed(rush_, "rush", rush);

        if (aloud_) {
            audio_.setListener(eye, forward, {0, 1, 0});
            const Vec3 close = eye + forward;
            audio_.setVoicePosition(roar_, close);
            audio_.setVoicePosition(crackle_, close);
            audio_.setVoicePosition(rush_, close);
        }

        if (swarm.ignited()) play(ignite_, "ignite", 0.9f);
        if (freshWake) play(wake_, "wake", 0.7f);

        if (story.gate() > 0.0f && !gateHeard_) {
            gateHeard_ = true;
            play(gate_, "gate", 1.0f);
            play(groan_, "groan", 0.5f);
        }
        if (story.pulling() && !spoolHeard_) {
            spoolHeard_ = true;
            play(spool_, "spool", 0.9f);
        }
        if (blown > 0.0f && !finaleHeard_) {
            finaleHeard_ = true;
            play(finale_, "finale", 1.0f);
            play(wake_, "wake", 1.0f);
        }

        // The machine shifts in its sleep, somewhere off in the dark.
        creakIn_ -= dt;
        if (creakIn_ <= 0.0f) {
            creakIn_ = random_.range(5.0f, 13.0f);
            const auto which = static_cast<std::size_t>(random_.range(0.0f, 4.99f));
            const Vec3 off{random_.range(-18.0f, 18.0f), random_.range(-4.0f, 5.0f), random_.range(-18.0f, 18.0f)};
            playFar(which, eye + off);
        }
    }

private:
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
        float gain;
        float pitch;
    };

    void setBed(iron::VoiceId voice, const char* name, Bed bed) {
        if (aloud_) {
            audio_.setVoiceGain(voice, bed.gain);
            audio_.setVoicePitch(voice, bed.pitch);
        }
        if (log_) std::fprintf(log_, "%.4f,bed,%s,%.4f,%.4f\n", time_, name, bed.gain, bed.pitch);
    }

    void play(iron::SoundHandle sound, const char* name, float gain) {
        if (aloud_) audio_.playSoundLocal(sound, gain);
        if (log_) std::fprintf(log_, "%.4f,shot,%s,%.4f,1\n", time_, name, gain);
    }

    void playFar(std::size_t which, Vec3 position) {
        static constexpr const char* kNames[5] = {"creak1", "creak2", "creak3", "chain", "bend"};
        // Mono voices fall off with distance, so this is loud at the source.
        if (aloud_) audio_.playSoundAt(creaks_[which], position, 7.0f);
        if (log_) std::fprintf(log_, "%.4f,shot,%s,0.35,1\n", time_, kNames[which]);
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
    iron::SoundHandle creaks_[5] = {iron::kInvalidSound, iron::kInvalidSound, iron::kInvalidSound,
                                    iron::kInvalidSound, iron::kInvalidSound};

    iron::VoiceId room_ = iron::kInvalidVoice;
    iron::VoiceId roar_ = iron::kInvalidVoice;
    iron::VoiceId crackle_ = iron::kInvalidVoice;
    iron::VoiceId rush_ = iron::kInvalidVoice;

    bool gateHeard_ = false;
    bool spoolHeard_ = false;
    bool finaleHeard_ = false;
    float creakIn_ = 3.0f;
    Random random_;
};

}  // namespace kindle
