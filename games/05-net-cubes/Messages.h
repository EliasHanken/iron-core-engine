#pragma once

#include <cstdint>

namespace iron::netcubes {

struct HelloMsg {
    static constexpr std::uint8_t kTag = 1;
    std::uint32_t peerId;
};

struct PositionMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t peerId;
    float x, y, z;
};

} // namespace iron::netcubes
