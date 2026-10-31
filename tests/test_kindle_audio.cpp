// tests/test_kindle_audio.cpp — every sound Kindle needs exists and can be decoded.

#include "19-kindle/Sound.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

int main() {
    const std::string folder = std::string(IRON_REPO_ROOT) + "/games/19-kindle/assets/audio/";
    for (const char* name : {"room", "roar", "crackle", "rush", "ignite", "wake", "gate", "groan", "spool", "finale",
                             "creak1", "creak2", "creak3", "chain", "bend"}) {
        const std::string path = folder + name + ".wav";
        assert(std::filesystem::exists(path));
        // An LFS pointer that was never pulled is a tiny text file.
        assert(std::filesystem::file_size(path) > 10000);
    }

    // A pack round-trips, and a damaged one reads as empty rather than crashing.
    {
        const std::string path = "test_kindle.pak";
        std::vector<std::uint8_t> wav = {1, 2, 3, 250};
        std::vector<std::uint8_t> scrambled = wav;
        kindle::scramble(scrambled.data(), scrambled.size());
        std::FILE* file = std::fopen(path.c_str(), "wb");
        const std::uint8_t head[] = {'K', 'P', 'A', 'K', '1', 1, 0, 0, 0, 2, 0, 'a', 'b', 4, 0, 0, 0};
        std::fwrite(head, 1, sizeof(head), file);
        std::fwrite(scrambled.data(), 1, scrambled.size(), file);
        std::fclose(file);
        const auto read = kindle::readAudioPak(path);
        assert(read.count("ab") == 1);
        assert(read.at("ab") == wav);

        file = std::fopen(path.c_str(), "wb");
        std::fwrite(head, 1, sizeof(head) - 2, file);
        std::fclose(file);
        assert(kindle::readAudioPak(path).empty());
        std::remove(path.c_str());
        assert(kindle::readAudioPak(path).empty());
    }

    iron::AudioEngine audio;
    if (audio.init()) {  // There may be no audio device on a build machine.
        // A sound decodes from memory just as it does from a file.
        std::FILE* wavFile = std::fopen((folder + "bend.wav").c_str(), "rb");
        assert(wavFile);
        std::vector<std::uint8_t> bytes(std::filesystem::file_size(folder + "bend.wav"));
        assert(std::fread(bytes.data(), 1, bytes.size(), wavFile) == bytes.size());
        std::fclose(wavFile);
        assert(audio.loadSoundFromMemory(bytes.data(), bytes.size(), "bend") != iron::kInvalidSound);
        assert(audio.loadSoundFromMemory(bytes.data(), 8, "cut short") == iron::kInvalidSound);

        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (entry.path().extension() == ".wav") {
                assert(audio.loadSound(entry.path().string()) != iron::kInvalidSound);
            }
        }
    }

    // Starting and stopping every voice at silence must also be safe.
    {
        kindle::Sound sound;
        sound.init(folder, folder + "no-such-folder/", true, "");
    }

    std::puts("test_kindle_audio: all passed");
    return 0;
}
