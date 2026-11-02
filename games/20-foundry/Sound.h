#pragma once

// Everything Foundry plays, and how it's mixed. The licensed set (tools/foundry/
// build_audio_sonniss.py) comes from the packed audio.pak first, then from a local
// build's audio-hq folder. Without either, the CC0 shooter set stands in where it can,
// and the rest stays silent.
//
// The rifle is layered like a AAA weapon: a dry shot per round (base, punch, crack and
// action, built by the tool), a short tail of the space with every round, and the
// space's longer tail when a burst ends. Two spaces, the open yard and the hall, crossfade by how far
// inside the player is: the ambience beds, the gun's tail and the reverb.

#include "Level.h"

#include "audio/AudioEngine.h"
#include "common/AudioPak.h"
#include "math/Vec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace foundry {

// What's underfoot.
enum class Floor : std::uint8_t { Ground, Concrete, Metal };

class Sound {
public:
    // Everything sits below full scale so that a burst stays clean.
    static constexpr float kHeadroom = 0.5f;

    // `assets` is the game's assets folder, with a trailing slash.
    void init(const std::string& assets, Vec3 furnace) {
        aloud_ = audio_.init();
        if (!aloud_) return;
        pak_ = audiopak::readAudioPak(assets + "audio.pak");
        hq_ = assets + "audio-hq/";
        cc0_ = assets + "sfx/";

        for (int i = 0; i < 5; ++i) shots_.push_back(load("shot" + std::to_string(i + 1), "gunshot-rifle"));
        for (int i = 0; i < 3; ++i) {
            shotTailsHall_.push_back(load("tailshot-hall" + std::to_string(i + 1)));
            shotTailsYard_.push_back(load("tailshot-yard" + std::to_string(i + 1)));
        }
        tailHall_ = load("tail-hall");
        tailYard_ = load("tail-yard");
        magOut_ = load("mag-out");
        magIn_ = load("mag-in");
        charge_ = load("charge");
        for (const char* n : {"casing1", "casing2", "casing3"}) casings_.push_back(load(n));
        hits_[static_cast<int>(Surface::Metal)] = {load("hit-metal1", "hit"), load("hit-metal2", "hit")};
        hits_[static_cast<int>(Surface::Concrete)] = {load("hit-concrete1", "hit"), load("hit-concrete2", "hit"),
                                                      load("hit-concrete3", "hit")};
        hits_[static_cast<int>(Surface::Brick)] = hits_[static_cast<int>(Surface::Concrete)];
        hits_[static_cast<int>(Surface::Wood)] = {load("hit-wood1", "hit"), load("hit-wood2", "hit")};
        ricochet_ = load("ricochet");
        for (const char* n : {"plate1", "plate2", "plate3"}) plates_.push_back(load(n, "hit"));
        steps_[static_cast<int>(Floor::Ground)] = {load("step-ground1", "footstep-01"), load("step-ground2", "footstep-02")};
        steps_[static_cast<int>(Floor::Concrete)] = {load("step-concrete1", "footstep-03"),
                                                     load("step-concrete2", "footstep-04")};
        steps_[static_cast<int>(Floor::Metal)] = {load("step-metal1", "footstep-01"), load("step-metal2", "footstep-03")};
        for (const char* n : {"works1", "works2", "works3"}) works_.push_back(load(n));
        blast_ = load("blast", "rocket-explode");
        debris_ = load("debris");
        bounce_ = load("bounce", "footstep-02");

        bedYard_ = audio_.playLooping(load("bed-yard"), Vec3{}, 0.0f);
        bedHall_ = audio_.playLooping(load("bed-hall"), Vec3{}, 0.0f);
        furnace_ = audio_.playLooping(load("furnace"), furnace, 0.0f);
    }

    bool licensed() const { return licensed_; }

    // Once a frame. `indoor` is 0 in the yard and 1 deep in the hall.
    void update(float dt, Vec3 eye, Vec3 forward, float indoor, float seconds) {
        if (!aloud_) return;
        audio_.setListener(eye, forward, {0.0f, 1.0f, 0.0f});
        indoor_ = indoor;

        // A burst ends when no round has left for longer than the action cycles.
        sinceShot_ += dt;
        if (burst_ > 0 && sinceShot_ > 0.12f) {
            const float size = std::min(static_cast<float>(burst_), 6.0f) / 6.0f;
            const float gain = kHeadroom * 0.8f * (0.55f + 0.45f * size);
            if (tailHall_) audio_.playSoundLocal(tailHall_, gain * indoor, 0.98f + random() * 0.04f);
            if (tailYard_) audio_.playSoundLocal(tailYard_, gain * 1.2f * (1.0f - indoor), 0.98f + random() * 0.04f);
            burst_ = 0;
        }

        // What was waiting to be heard: debris coming down after a blast.
        for (Later& l : later_) l.in -= dt;
        for (const Later& l : later_) {
            if (l.in <= 0.0f && l.sound) audio_.playSoundAt(l.sound, l.at, l.gain, l.pitch);
        }
        std::erase_if(later_, [](const Later& l) { return l.in <= 0.0f; });

        // Firing pushes the beds down a little, and they come back.
        duck_ = std::max(duck_ - dt * 1.5f, 0.0f);
        const float bed = kHeadroom * (1.0f - 0.35f * std::min(duck_, 2.4f));
        audio_.setVoiceGain(bedYard_, bed * 0.9f * (1.0f - indoor));
        audio_.setVoiceGain(bedHall_, bed * (0.25f + 0.75f * indoor));
        audio_.setVoiceGain(furnace_, kHeadroom * 1.6f);

        // The works, now and then, somewhere down the hall.
        worksIn_ -= dt;
        if (worksIn_ <= 0.0f && !works_.empty()) {
            worksIn_ = 7.0f + random() * 12.0f;
            const Vec3 at{random() * 28.0f - 14.0f, 3.0f, -24.0f - random() * 38.0f};
            const auto h = works_[static_cast<std::size_t>(random() * 2.99f)];
            if (h) audio_.playSoundAt(h, at, kHeadroom * 0.8f * (0.3f + 0.7f * indoor), 0.85f + random() * 0.3f);
        }

        // The yard is open air with a few hard walls; the hall is a long brick box.
        iron::ReverbSettings want;
        want.decaySeconds = 0.9f + 2.2f * indoor;
        want.gain = 0.12f + 0.3f * indoor;
        want.gainHF = 0.75f - 0.25f * indoor;
        want.reflectionsGain = 0.03f + 0.12f * indoor;
        want.reflectionsDelay = 0.02f - 0.01f * indoor;
        want.lateDelay = 0.03f - 0.015f * indoor;
        const float k = 1.0f - std::exp(-dt * 3.0f);
        const auto ease = [&](float& v, float w) { v += (w - v) * k; };
        ease(reverb_.decaySeconds, want.decaySeconds);
        ease(reverb_.gain, want.gain);
        ease(reverb_.gainHF, want.gainHF);
        ease(reverb_.reflectionsGain, want.reflectionsGain);
        ease(reverb_.reflectionsDelay, want.reflectionsDelay);
        ease(reverb_.lateDelay, want.lateDelay);
        reverbIn_ -= dt;
        if (reverbIn_ <= 0.0f) {
            audio_.setReverb(reverb_);
            reverbIn_ = 0.1f;
        }
        (void)seconds;
    }

    void shot() {
        if (!aloud_ || shots_.empty()) return;
        // Never the same take twice in a row.
        std::size_t pick = static_cast<std::size_t>(random() * (static_cast<float>(shots_.size()) - 0.01f));
        if (pick == lastShot_) pick = (pick + 1) % shots_.size();
        lastShot_ = pick;
        if (shots_[pick]) audio_.playSoundLocal(shots_[pick], kHeadroom * 1.0f, 0.97f + random() * 0.06f);
        // The space answering this round: the hall and the yard by how far in the player is.
        const std::size_t tail = static_cast<std::size_t>(random() * 2.99f);
        if (tail < shotTailsHall_.size() && shotTailsHall_[tail] && indoor_ > 0.02f) {
            audio_.playSoundLocal(shotTailsHall_[tail], kHeadroom * 0.5f * indoor_, 0.97f + random() * 0.06f);
        }
        if (tail < shotTailsYard_.size() && shotTailsYard_[tail] && indoor_ < 0.98f) {
            audio_.playSoundLocal(shotTailsYard_[tail], kHeadroom * 0.55f * (1.0f - indoor_), 0.97f + random() * 0.06f);
        }
        ++burst_;
        sinceShot_ = 0.0f;
        duck_ = 1.0f;
    }

    // The weapon's handling sounds, on the animation's events: the magazine out
    // and in, the bolt racked back and let fly, a hand taking hold.
    void foley(std::string_view event) {
        if (!aloud_) return;
        iron::SoundHandle h = iron::kInvalidSound;
        float gain = 0.85f, pitch = 0.98f + random() * 0.04f;
        if (event == "mag_out") h = magOut_;
        else if (event == "mag_in") h = magIn_;
        else if (event == "bolt_back") h = charge_;
        else if (event == "bolt_forward") {
            h = charge_;
            pitch *= 1.18f;
            gain = 0.6f;
        } else if (event == "grab" || event == "draw") {
            h = charge_;
            pitch *= 0.62f;
            gain = 0.18f;
        }
        if (h) audio_.playSoundLocal(h, kHeadroom * gain, pitch);
    }

    void impact(Surface surface, bool plate, Vec3 at) {
        if (!aloud_) return;
        const float pitch = 0.92f + random() * 0.16f;
        if (plate && !plates_.empty()) {
            const auto h = plates_[static_cast<std::size_t>(random() * (static_cast<float>(plates_.size()) - 0.01f))];
            if (h) audio_.playSoundAt(h, at, kHeadroom * 1.4f, pitch);
            return;
        }
        const auto& set = hits_[static_cast<int>(surface)];
        if (!set.empty()) {
            const auto h = set[static_cast<std::size_t>(random() * (static_cast<float>(set.size()) - 0.01f))];
            if (h) audio_.playSoundAt(h, at, kHeadroom * 1.1f, pitch);
        }
        if (surface == Surface::Metal && ricochet_ && random() < 0.18f) {
            audio_.playSoundAt(ricochet_, at, kHeadroom * 0.7f, 0.9f + random() * 0.2f);
        }
    }

    // A grenade going off `distance` metres away: the blast, the beds pushed right
    // down, the space's tail, then what it threw coming back down.
    void blast(Vec3 at, float distance) {
        if (!aloud_) return;
        const float pitch = 0.94f + random() * 0.1f;
        if (blast_) audio_.playSoundAt(blast_, at, kHeadroom * 2.4f, pitch);
        const float near = std::clamp(1.0f - distance / 40.0f, 0.0f, 1.0f);
        if (tailHall_ && indoor_ > 0.05f) audio_.playSoundLocal(tailHall_, kHeadroom * 1.2f * indoor_ * near, 0.7f);
        if (tailYard_ && indoor_ < 0.95f) {
            audio_.playSoundLocal(tailYard_, kHeadroom * 1.4f * (1.0f - indoor_) * near, 0.72f);
        }
        if (debris_) later_.push_back({0.35f + random() * 0.2f, debris_, at, kHeadroom * 1.3f, 0.9f + random() * 0.2f});
        duck_ = 3.0f;
    }

    // A grenade landing, hard (1) or softly.
    void bounce(Vec3 at, float hardness) {
        if (!aloud_ || !bounce_) return;
        audio_.playSoundAt(bounce_, at, kHeadroom * (0.3f + 0.7f * std::clamp(hardness, 0.0f, 1.0f)),
                           0.9f + random() * 0.25f);
    }

    void casing(Vec3 at) {
        if (!aloud_ || casings_.empty()) return;
        const auto h = casings_[static_cast<std::size_t>(random() * (static_cast<float>(casings_.size()) - 0.01f))];
        if (h) audio_.playSoundAt(h, at, kHeadroom * 0.55f, 0.9f + random() * 0.25f);
    }

    void step(Floor floor, Vec3 at, float loudness) {
        if (!aloud_) return;
        const auto& set = steps_[static_cast<int>(floor)];
        if (set.empty()) return;
        std::size_t pick = static_cast<std::size_t>(random() * (static_cast<float>(set.size()) - 0.01f));
        if (pick == lastStep_ && set.size() > 1) pick = (pick + 1) % set.size();
        lastStep_ = pick;
        if (set[pick]) audio_.playSoundAt(set[pick], at, kHeadroom * loudness, 0.92f + random() * 0.14f);
    }

private:
    iron::SoundHandle load(const std::string& name, const std::string& fallback = "") {
        const auto packed = pak_.find(name);
        if (packed != pak_.end()) {
            licensed_ = true;
            return audio_.loadSoundFromMemory(packed->second.data(), packed->second.size(), name);
        }
        const std::string hq = hq_ + name + ".wav";
        if (std::filesystem::exists(hq)) {
            licensed_ = true;
            return audio_.loadSound(hq);
        }
        if (!fallback.empty()) return audio_.loadSound(cc0_ + fallback + ".wav");
        return iron::kInvalidSound;
    }

    float random() {
        seed_ = seed_ * 1664525u + 1013904223u;
        return static_cast<float>(seed_ >> 8) * (1.0f / 16777216.0f);
    }

    iron::AudioEngine audio_;
    bool aloud_ = false;
    bool licensed_ = false;
    std::map<std::string, std::vector<std::uint8_t>> pak_;
    std::string hq_;
    std::string cc0_;
    std::uint32_t seed_ = 0x2545f491u;

    std::vector<iron::SoundHandle> shots_;
    iron::SoundHandle tailHall_ = iron::kInvalidSound;
    iron::SoundHandle tailYard_ = iron::kInvalidSound;
    iron::SoundHandle magOut_ = iron::kInvalidSound;
    iron::SoundHandle magIn_ = iron::kInvalidSound;
    iron::SoundHandle charge_ = iron::kInvalidSound;
    iron::SoundHandle ricochet_ = iron::kInvalidSound;
    iron::SoundHandle blast_ = iron::kInvalidSound;
    iron::SoundHandle debris_ = iron::kInvalidSound;
    iron::SoundHandle bounce_ = iron::kInvalidSound;
    struct Later {
        float in;
        iron::SoundHandle sound;
        Vec3 at;
        float gain;
        float pitch;
    };
    std::vector<Later> later_;
    std::vector<iron::SoundHandle> shotTailsHall_;
    std::vector<iron::SoundHandle> shotTailsYard_;
    std::vector<iron::SoundHandle> casings_;
    std::vector<iron::SoundHandle> plates_;
    std::vector<iron::SoundHandle> works_;
    std::array<std::vector<iron::SoundHandle>, static_cast<int>(Surface::Count)> hits_;
    std::array<std::vector<iron::SoundHandle>, 3> steps_;
    iron::VoiceId bedYard_{};
    iron::VoiceId bedHall_{};
    iron::VoiceId furnace_{};

    std::size_t lastShot_ = 99;
    std::size_t lastStep_ = 99;
    int burst_ = 0;
    float sinceShot_ = 10.0f;
    float duck_ = 0.0f;
    float indoor_ = 0.0f;
    float worksIn_ = 6.0f;
    iron::ReverbSettings reverb_;
    float reverbIn_ = 0.0f;
};

}  // namespace foundry
