#pragma once

#include "math/Vec.h"

#include <cstdint>

namespace iron::netshooter {

// "NSTR" — net shooter. PeerManager validates this on Hello.
constexpr std::uint32_t kGameId = 0x4E535452u;

// Player AABB half-extents (0.8 m wide, 2 m tall). Defined here so host
// and client agree; used by LagCompensator + splash damage.
constexpr Vec3 kPlayerHalfExtents = Vec3{0.4f, 1.0f, 0.4f};

// Client interpolation delay — clients render remote players from
// TimeHistory at (renderNow - kInterpDelaySec). Same value drives
// viewTimeSec when firing.
constexpr double kInterpDelaySec = 0.10;

// Game tags. Tag 1 is HelloMsg (engine-owned). 254/255 are Ping/Pong
// (engine-owned). Game tags must live in [2, 253].

// Client -> Host: input tick.
struct PlayerInputMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t inputId;
    float dx, dy, dz;
};

// Client -> Host: hitscan shot.
struct FireHitscanMsg {
    static constexpr std::uint8_t kTag = 3;
    float ox, oy, oz;
    float dx, dy, dz;
    double viewTimeSec;
};

// Client -> Host: rocket shot.
struct FireRocketMsg {
    static constexpr std::uint8_t kTag = 4;
    float ox, oy, oz;
    float dx, dy, dz;
    double viewTimeSec;
};

// Host -> Clients: authoritative position broadcast.
struct AuthorityPositionMsg {
    static constexpr std::uint8_t kTag = 5;
    std::uint32_t peerId;
    float x, y, z;
    std::uint32_t lastInputId;
};

// Host -> Clients: rocket spawned.
struct SpawnProjectileMsg {
    static constexpr std::uint8_t kTag = 6;
    std::uint32_t projectileId;
    std::uint32_t ownerPeerId;
    float x, y, z;
    float vx, vy, vz;
    double spawnTimeSec;
};

// Host -> Clients: rocket destroyed (hit, expired, etc.).
struct DespawnProjectileMsg {
    static constexpr std::uint8_t kTag = 7;
    std::uint32_t projectileId;
    float x, y, z;
};

// Host -> Clients: damage applied. Broadcast so everyone can render the
// kill feed.
struct DamageMsg {
    static constexpr std::uint8_t kTag = 8;
    std::uint32_t attackerPeerId;
    std::uint32_t victimPeerId;
    std::uint16_t damage;
    std::uint16_t victimHpAfter;  // 0 means killed by this hit
};

// Host -> Clients: player respawned after 2 s timer.
struct RespawnMsg {
    static constexpr std::uint8_t kTag = 9;
    std::uint32_t peerId;
    float x, y, z;
    std::uint16_t hp;
};

// Host -> Clients: score update for one peer.
struct ScoreUpdateMsg {
    static constexpr std::uint8_t kTag = 10;
    std::uint32_t peerId;
    std::uint32_t kills;
    std::uint32_t deaths;
};

}  // namespace iron::netshooter
