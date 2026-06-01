#pragma once

#include <string>

namespace iron {

// Authorable sound source on an entity. On Edit->Play the host loads the WAV
// and starts a voice at the entity (looping/positional per the flags); the
// voice is stopped on Stop. One-shot event sounds (gunshots etc.) belong in
// gameplay code, not here — an emitter is for persistent/ambient sound.
struct AudioEmitter {
    std::string wavPath;
    float       gain        = 1.0f;
    bool        loop        = true;   // looping ambience vs. one-shot on start
    bool        spatial     = true;   // positional (3D) vs. 2D (centered)
    bool        playOnStart = true;   // auto-start on Edit->Play
};

}  // namespace iron
