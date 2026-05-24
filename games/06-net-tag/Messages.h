#pragma once

#include <cstdint>

namespace iron::nettag {

// 4-byte ASCII game identifier. 't', 'A', 'G', 'o' → 0x7441'476F.
constexpr std::uint32_t kGameId = 0x7441476Fu;

// Client → host each input frame. dx/dy/dz are movement delta in world
// coordinates (client has computed from WASD + yaw + speed-per-tick).
struct PlayerInputMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t inputId;
    float dx, dy, dz;
};

// Host → all peers; broadcasts the authoritative position. lastInputId
// lets the sender's client reconcile its prediction. lastInputId is 0
// for the host's own peer (no client to reconcile).
struct AuthorityPositionMsg {
    static constexpr std::uint8_t kTag = 3;
    std::uint32_t peerId;
    float x, y, z;
    std::uint32_t lastInputId;
};

struct TagSwapMsg {
    static constexpr std::uint8_t kTag = 4;
    std::uint32_t newItPeerId;
};

struct ScoreUpdateMsg {
    static constexpr std::uint8_t kTag = 5;
    std::uint32_t peerId;
    float itTimeSec;
};

struct RoundStartMsg {
    static constexpr std::uint8_t kTag = 6;
    std::uint32_t initialItPeerId;
    float roundDurationSec;
};

struct RoundEndMsg {
    static constexpr std::uint8_t kTag = 7;
    std::uint32_t winnerPeerId;
};

}  // namespace iron::nettag
