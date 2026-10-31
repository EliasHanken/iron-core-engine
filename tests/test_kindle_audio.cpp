// tests/test_kindle_audio.cpp — every sound Kindle needs exists and can be decoded.

#include "19-kindle/Sound.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

int main() {
    const std::string folder = std::string(IRON_REPO_ROOT) + "/games/19-kindle/assets/audio/";
    for (const char* name : {"room", "roar", "crackle", "rush", "ignite", "wake", "gate", "groan", "spool", "finale",
                             "creak1", "creak2", "creak3", "chain", "bend"}) {
        const std::string path = folder + name + ".wav";
        assert(std::filesystem::exists(path));
        // An LFS pointer that was never pulled is a tiny text file.
        assert(std::filesystem::file_size(path) > 10000);
    }

    iron::AudioEngine audio;
    if (audio.init()) {  // There may be no audio device on a build machine.
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (entry.path().extension() == ".wav") {
                assert(audio.loadSound(entry.path().string()) != iron::kInvalidSound);
            }
        }
    }

    // Starting and stopping every voice at silence must also be safe.
    {
        kindle::Sound sound;
        sound.init(folder, true, "");
    }

    std::puts("test_kindle_audio: all passed");
    return 0;
}
