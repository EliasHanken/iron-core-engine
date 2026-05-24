#pragma once

#include <cstdint>

namespace iron::nettag {

// 4-byte ASCII game identifier. 't', 'A', 'G', 'o' → 0x7441'476F.
// Sent in every HelloMsg; client rejects a Hello whose gameId doesn't
// match. Prevents this game from being accidentally connected to a
// different iron-core network exe (e.g. net-cubes).
constexpr std::uint32_t kGameId = 0x7441476Fu;

struct HelloMsg {
    static constexpr std::uint8_t kTag = 1;
    std::uint32_t gameId;   // = kGameId on send; receiver rejects on mismatch
    std::uint32_t peerId;
};

struct PositionMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t peerId;
    float x, y, z;
};

struct TagSwapMsg {
    static constexpr std::uint8_t kTag = 3;
    std::uint32_t newItPeerId;
};

struct ScoreUpdateMsg {
    static constexpr std::uint8_t kTag = 4;
    std::uint32_t peerId;
    float itTimeSec;
};

struct RoundStartMsg {
    static constexpr std::uint8_t kTag = 5;
    std::uint32_t initialItPeerId;
    float roundDurationSec;
};

struct RoundEndMsg {
    static constexpr std::uint8_t kTag = 6;
    std::uint32_t winnerPeerId;
};

}  // namespace iron::nettag
