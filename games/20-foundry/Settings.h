#pragma once

// What the player can set from the pause menu, kept in foundry.settings next to
// the game: a line of `name value` each.

#include "core/Platform.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace foundry {

struct Settings {
    float sensitivity = 1.0f;  // Times the mouse's base speed.
    float volume = 1.0f;       // Master, 0..1.
    float fovDegrees = 60.0f;  // Vertical, at the hip.
    int   difficulty = 1;      // 0 recruit, 1 regular, 2 veteran.

    static constexpr const char* kDifficulty[3] = {"RECRUIT", "REGULAR", "VETERAN"};
    // How hard rounds hit the player, and how well the soldiers shoot.
    float damageTaken() const { return difficulty == 0 ? 0.55f : difficulty == 2 ? 1.5f : 1.0f; }
    float enemyAccuracy() const { return difficulty == 0 ? 0.7f : difficulty == 2 ? 1.35f : 1.0f; }

    static std::string path() { return iron::executableDir() + "/foundry.settings"; }

    void load() {
        std::FILE* f = std::fopen(path().c_str(), "r");
        if (!f) return;
        char name[32];
        float value = 0.0f;
        while (std::fscanf(f, "%31s %f", name, &value) == 2) {
            const std::string n = name;
            if (n == "sensitivity") sensitivity = std::clamp(value, 0.2f, 3.0f);
            else if (n == "volume") volume = std::clamp(value, 0.0f, 1.0f);
            else if (n == "fov") fovDegrees = std::clamp(value, 50.0f, 80.0f);
            else if (n == "difficulty") difficulty = std::clamp(static_cast<int>(value), 0, 2);
        }
        std::fclose(f);
    }
    void save() const {
        std::FILE* f = std::fopen(path().c_str(), "w");
        if (!f) return;
        std::fprintf(f, "sensitivity %.2f\nvolume %.2f\nfov %.0f\ndifficulty %d\n", sensitivity, volume, fovDegrees,
                     difficulty);
        std::fclose(f);
    }
};

}  // namespace foundry
