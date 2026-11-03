#pragma once

// The demo: a dawn raid on the steelworks, ten to twenty minutes.
//
//   Title     the yard at dawn from above, slowly turning; Enter to begin.
//   Intro     the camera comes down over the yard to the player at the south
//             wall; captions; skippable.
//   Infiltrate  get across the yard to the bunker hatch: sneak past the patrols
//             or fight through them.
//   Intel     find the plant's schematics in the bunker. Taking them trips the
//             alarm: everyone's up, and a team comes out of the hall.
//   Breach    climb out and get into the hall.
//   Hall      clear it: its defenders, one up on the catwalk.
//   Charge    set a charge on the furnace. Seventy-five seconds.
//   Escape    out to the extraction at the south wall, through what's come into
//             the yard. If the timer runs out the hall goes up, with whoever's in it.
//   Outro     from the extraction, the hall going up; the mission's numbers.
//
// The game owns the world: this keeps the story, and asks the game to act on
// it through hooks. A checkpoint is taken as each stage begins; dying puts the
// player back at the last one.

#include "render/CameraTrack.h"
#include "math/Vec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace foundry {

using iron::Vec3;

class Mission {
public:
    struct Checkpoint {
        Vec3  feet;
        float yaw;
    };

    enum class Stage { Title, Intro, Infiltrate, Intel, Breach, Hall, Charge, Escape, Outro, Done };

    struct Places {
        Vec3  start;        // Where the player begins (feet).
        float startYaw = 0.0f;
        Vec3  hatch;        // The bunker's hatch.
        float bunkerFloor = 0.0f;
        Vec3  intel;        // The schematics.
        Vec3  hallDoor;     // The middle of the hall's doorway, on the floor.
        float hallFront = 0.0f;   // The hall's front wall (z); inside is beyond it.
        float hallHalfWidth = 0.0f;
        Vec3  furnace;      // Where the charge goes.
        Vec3  extraction;
    };

    // What the game does when the story moves on.
    struct Hooks {
        std::function<void()>    alarm;          // The schematics are taken.
        std::function<int()>     hallLeft;       // Soldiers alive in the hall.
        std::function<void()>    chargeSet;      // The timer's started.
        std::function<void()>    hallBlows;      // Time's up, or the outro: the hall goes up.
        std::function<void(Vec3)> extractionSmoke;
    };

    Mission(Places places, Hooks hooks) : p_(places), hooks_(std::move(hooks)) {
        checkpoint_ = {p_.start, p_.startYaw};
        intro_ = iron::CameraTrack({
            {0.0f, {34.0f, 22.0f, 52.0f}, {0.0f, 2.0f, -22.0f}},
            {5.0f, {14.0f, 11.0f, 44.0f}, {-6.0f, 1.0f, 0.0f}},
            {9.0f, p_.start + Vec3{4.0f, 4.0f, 6.0f}, p_.start + Vec3{0.0f, 1.2f, -8.0f}},
            {12.0f, p_.start + Vec3{0.0f, 1.65f, 0.0f},
             p_.start + Vec3{-std::sin(p_.startYaw), 1.65f, -std::cos(p_.startYaw)}},
        });
    }

    // Skip the title and intro (a capture starting partway, or straight in).
    void startAt(Stage stage) {
        stage_ = Stage::Infiltrate;
        if (stage >= Stage::Intel) enter(Stage::Intel);
        if (stage >= Stage::Breach) {
            intelTaken_ = true;
            enter(Stage::Breach);
        }
        if (stage >= Stage::Hall) enter(Stage::Hall);
        if (stage >= Stage::Charge) enter(Stage::Charge);
        if (stage >= Stage::Escape) enter(Stage::Escape);
        if (stage >= Stage::Outro) enter(Stage::Outro);
        if (stage == Stage::Infiltrate) enter(Stage::Infiltrate);
    }

    // --- Each frame ---
    // `use`: E this frame; `confirm`: Enter or Space this frame.
    void update(float dt, Vec3 feet, Vec3 eye, Vec3 aim, bool use, bool confirm, bool downed) {
        clock_ += dt;
        stageTime_ += dt;
        prompt_ = nullptr;
        if (stage_ >= Stage::Infiltrate && stage_ < Stage::Outro) played_ += dt;
        const auto facing = [&](Vec3 at, float reach, float cone) {
            const Vec3 to = at - eye;
            const float d = iron::length(to);
            return d < reach && iron::dot(aim, to * (1.0f / std::max(d, 1e-3f))) > cone;
        };
        const bool inHall = feet.z < p_.hallFront - 0.5f && std::abs(feet.x) < p_.hallHalfWidth && feet.y > -1.0f;
        switch (stage_) {
            case Stage::Title:
                if (confirm) enter(Stage::Intro);
                break;
            case Stage::Intro:
                if (confirm || stageTime_ > intro_.duration()) enter(Stage::Infiltrate);
                break;
            case Stage::Infiltrate:
                if (feet.y < p_.bunkerFloor + 0.5f) enter(Stage::Intel);
                break;
            case Stage::Intel:
                if (facing(p_.intel, 1.8f, 0.6f) && !downed) {
                    prompt_ = "E   TAKE THE SCHEMATICS";
                    if (use) {
                        intelTaken_ = true;
                        if (hooks_.alarm) hooks_.alarm();
                        enter(Stage::Breach);
                    }
                }
                break;
            case Stage::Breach:
                // A checkpoint once he's back up in the yard.
                if (!climbedOut_ && feet.y > -0.3f) {
                    climbedOut_ = true;
                    checkpoint_ = {p_.hatch + Vec3{0.0f, 0.0f, -1.3f}, 0.0f};
                }
                if (inHall) enter(Stage::Hall);
                break;
            case Stage::Hall:
                if (hooks_.hallLeft && hooks_.hallLeft() == 0) enter(Stage::Charge);
                break;
            case Stage::Charge:
                if (facing(p_.furnace, 2.4f, 0.5f) && !downed) {
                    prompt_ = "E   SET THE CHARGE";
                    if (use) enter(Stage::Escape);
                }
                break;
            case Stage::Escape:
                if (!blown_) {
                    timer_ -= dt;
                    if (timer_ <= 0.0f) {
                        blown_ = true;
                        if (hooks_.hallBlows) hooks_.hallBlows();
                    }
                }
                if (iron::length(Vec3{feet.x - p_.extraction.x, 0.0f, feet.z - p_.extraction.z}) < 4.0f && !downed) {
                    enter(Stage::Outro);
                }
                break;
            case Stage::Outro:
                if (!blown_ && stageTime_ > 2.2f) {
                    blown_ = true;
                    if (hooks_.hallBlows) hooks_.hallBlows();
                }
                if (stageTime_ > 14.0f || (stageTime_ > 6.0f && confirm)) enter(Stage::Done);
                break;
            case Stage::Done:
                break;
        }
        objectiveFlash_ = std::max(objectiveFlash_ - dt, 0.0f);
    }

    // The player died: back to the checkpoint. A charge that's set is set again
    // with the clock full, and the hall's whole again for the retry.
    Checkpoint respawn() {
        ++deaths_;
        if (stage_ == Stage::Escape) {
            timer_ = kChargeSeconds;
            blown_ = false;
        }
        return checkpoint_;
    }

    const Checkpoint& checkpoint() const { return checkpoint_; }

    // --- What to show ---
    Stage stage() const { return stage_; }
    bool controlsLocked() const {
        return stage_ == Stage::Title || stage_ == Stage::Intro || stage_ == Stage::Outro;
    }
    // A camera the story's holding, instead of the player's.
    bool camera(Vec3& eye, Vec3& target) const {
        if (stage_ == Stage::Title) {
            const float a = 0.4f + clock_ * 0.035f;
            eye = Vec3{std::sin(a) * 58.0f, 26.0f, -8.0f + std::cos(a) * 58.0f};
            target = {0.0f, 3.0f, -12.0f};
            return true;
        }
        if (stage_ == Stage::Intro) {
            intro_.sample(stageTime_, eye, target);
            return true;
        }
        if (stage_ == Stage::Outro || stage_ == Stage::Done) {
            // Up behind the extraction, looking back at the hall going up.
            const float rise = stage_ == Stage::Done ? 1.0f : std::min(stageTime_ / 4.0f, 1.0f);
            const float r = rise * rise * (3.0f - 2.0f * rise);
            eye = p_.extraction + Vec3{-3.0f + 1.5f * r, 1.7f + 4.0f * r, 2.0f + 3.0f * r};
            target = p_.hallDoor + Vec3{0.0f, 6.0f + 3.0f * r, 0.0f};
            return true;
        }
        return false;
    }
    float letterbox() const {
        switch (stage_) {
            case Stage::Intro: return std::clamp(intro_.duration() - stageTime_, 0.0f, 1.0f);
            case Stage::Outro: return std::min(stageTime_ / 0.8f, 1.0f);
            case Stage::Done: return 1.0f;
            default: return 0.0f;
        }
    }
    const char* objective() const { return objective_.empty() ? nullptr : objective_.c_str(); }
    float objectiveFlash() const { return objectiveFlash_; }  // 3 just set, down to 0.
    std::optional<Vec3> marker() const {
        switch (stage_) {
            case Stage::Infiltrate: return p_.hatch + Vec3{0.0f, 0.4f, 0.0f};
            case Stage::Intel: return p_.intel + Vec3{0.0f, 0.15f, 0.0f};
            case Stage::Breach: return p_.hallDoor + Vec3{0.0f, 1.5f, 0.0f};
            case Stage::Charge: return p_.furnace;
            case Stage::Escape: return p_.extraction + Vec3{0.0f, 1.0f, 0.0f};
            default: return std::nullopt;
        }
    }
    const char* prompt() const { return prompt_; }
    // Seconds left on the charge, or -1.
    float timer() const { return stage_ == Stage::Escape && !blown_ ? timer_ : -1.0f; }
    bool intelTaken() const { return intelTaken_; }

    struct Caption {
        std::string big, small, hint;
        float       alpha = 0.0f;
    };
    Caption caption() const {
        const auto fade = [&](float from, float to) {
            const float t = stageTime_;
            return std::clamp(std::min((t - from) / 0.8f, (to - t) / 0.8f), 0.0f, 1.0f);
        };
        if (stage_ == Stage::Title) {
            return {"FOUNDRY", "PRESS ENTER TO BEGIN", "", std::min(stageTime_ / 1.5f, 1.0f)};
        }
        if (stage_ == Stage::Intro) {
            if (stageTime_ < 4.6f) return {"ZAVOD 7 STEELWORKS", "THE URALS  /  NOVEMBER", "", fade(0.6f, 4.6f)};
            if (stageTime_ < 9.0f) return {"05:42", "DAWN RAID", "", fade(5.0f, 9.0f)};
            return {};
        }
        if (stage_ == Stage::Outro || stage_ == Stage::Done) {
            const float a = std::clamp((stageTime_ - 4.5f) / 1.0f, 0.0f, 1.0f);
            const int minutes = static_cast<int>(played_) / 60;
            const int secs = static_cast<int>(played_) % 60;
            char line[160];
            std::snprintf(line, sizeof(line), "TIME %d:%02d     KILLS %d     ACCURACY %d%%     DEATHS %d", minutes, secs,
                          kills, shots > 0 ? static_cast<int>(100.0f * static_cast<float>(hits) / shots) : 0, deaths_);
            return {"MISSION COMPLETE", line, stage_ == Stage::Done ? "ENTER   PLAY AGAIN        ESC  Q   QUIT" : "",
                    stage_ == Stage::Done ? 1.0f : a};
        }
        return {};
    }

    // The player's numbers, kept by the game.
    int shots = 0, hits = 0, kills = 0;

private:
    static constexpr float kChargeSeconds = 75.0f;

    void enter(Stage s) {
        stage_ = s;
        stageTime_ = 0.0f;
        const auto set = [&](const char* text) {
            objective_ = text;
            objectiveFlash_ = 3.0f;
        };
        switch (s) {
            case Stage::Infiltrate:
                set("REACH THE BUNKER HATCH");
                checkpoint_ = {p_.start, p_.startYaw};
                break;
            case Stage::Intel:
                set("FIND THE PLANT SCHEMATICS");
                checkpoint_ = {p_.hatch + Vec3{0.0f, p_.bunkerFloor, 0.9f}, 3.14159265f};
                break;
            case Stage::Breach:
                set("GET INTO THE HALL");
                break;
            case Stage::Hall:
                set("CLEAR THE HALL");
                checkpoint_ = {p_.hallDoor + Vec3{0.0f, 0.0f, 1.5f}, 0.0f};
                break;
            case Stage::Charge:
                set("SET THE CHARGE ON THE FURNACE");
                checkpoint_ = {p_.hallDoor + Vec3{0.0f, 0.0f, -4.0f}, 0.0f};
                break;
            case Stage::Escape:
                set("GET OUT  /  EXTRACTION AT THE SOUTH WALL");
                timer_ = kChargeSeconds;
                checkpoint_ = {p_.furnace + Vec3{0.0f, -p_.furnace.y, 2.5f}, 3.14159265f};
                if (hooks_.chargeSet) hooks_.chargeSet();
                if (hooks_.extractionSmoke) hooks_.extractionSmoke(p_.extraction);
                break;
            case Stage::Outro:
                objective_.clear();
                break;
            default:
                break;
        }
    }

    Places      p_;
    Hooks       hooks_;
    Stage       stage_ = Stage::Title;
    float       clock_ = 0.0f;
    float       stageTime_ = 0.0f;
    float       played_ = 0.0f;
    std::string objective_;
    float       objectiveFlash_ = 0.0f;
    const char* prompt_ = nullptr;
    Checkpoint  checkpoint_{};
    float       timer_ = kChargeSeconds;
    bool        blown_ = false;
    bool        intelTaken_ = false;
    bool        climbedOut_ = false;
    int         deaths_ = 0;
    iron::CameraTrack intro_;
};

}  // namespace foundry
