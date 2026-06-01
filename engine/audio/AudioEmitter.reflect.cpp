#include "audio/AudioEmitter.h"
#include "reflection/Reflection.h"

namespace iron {

void registerAudioEmitter(Reflection& r) {
    r.registerType<AudioEmitter>("AudioEmitter")
        .field("wavPath",     &AudioEmitter::wavPath)
        .field("gain",        &AudioEmitter::gain, {.min = 0.0f, .max = 2.0f, .slider = true})
        .field("loop",        &AudioEmitter::loop)
        .field("spatial",     &AudioEmitter::spatial)
        .field("playOnStart", &AudioEmitter::playOnStart);
}

}  // namespace iron
