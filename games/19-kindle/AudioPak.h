#pragma once

// The licensed sounds travel inside one scrambled file rather than as loose WAVs. The
// licence forbids handing them out as sound effects, so they shouldn't sit in a game
// folder for anyone to copy. This deters casual copying. It isn't encryption.
// Written by tools/kindle/package.py.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace kindle {

inline void scramble(std::uint8_t* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        data[i] ^= static_cast<std::uint8_t>((0x5A + i * 31) & 0xFF);
    }
}

// Name to WAV bytes. Empty if the file is missing or damaged.
inline std::map<std::string, std::vector<std::uint8_t>> readAudioPak(const std::string& path) {
    std::map<std::string, std::vector<std::uint8_t>> out;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return out;

    const auto fail = [&]() {
        std::fclose(file);
        out.clear();
        return out;
    };
    const auto number = [&](int bytes) {
        std::uint32_t value = 0;
        for (int i = 0; i < bytes; ++i) {
            const int c = std::fgetc(file);
            if (c == EOF) return UINT32_MAX;
            value |= static_cast<std::uint32_t>(c) << (i * 8);
        }
        return value;
    };

    char magic[5] = {};
    if (std::fread(magic, 1, 5, file) != 5 || std::memcmp(magic, "KPAK1", 5) != 0) return fail();
    const std::uint32_t count = number(4);
    if (count == UINT32_MAX) return fail();
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t nameSize = number(2);
        if (nameSize == UINT32_MAX || nameSize > 255) return fail();
        std::string name(nameSize, '\0');
        if (std::fread(name.data(), 1, nameSize, file) != nameSize) return fail();
        const std::uint32_t size = number(4);
        if (size == UINT32_MAX) return fail();
        std::vector<std::uint8_t> data(size);
        if (std::fread(data.data(), 1, size, file) != size) return fail();
        scramble(data.data(), data.size());
        out[name] = std::move(data);
    }
    std::fclose(file);
    return out;
}

}  // namespace kindle
