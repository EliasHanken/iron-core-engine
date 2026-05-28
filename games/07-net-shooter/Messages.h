#pragma once

#include "math/Vec.h"

#include <cstdint>

namespace iron::netshooter {

// "NSTR" — net shooter. PeerManager validates this on Hello.
// M19 bump (NSTS = 0x4E535453) — wire format break (velocity + jump in
// PlayerInputMsg; velocity + grounded in AuthorityPositionMsg).
// M21 bump (NSTT = 0x4E535454) — adds DeathMsg (tag 11) for ragdolls.
// M25 bump (NSTU = 0x4E535455) — adds `yaw` to PlayerInputMsg and
// AuthorityPositionMsg so remote skinned-character facing tracks the
// player's look direction (not just velocity).
constexpr std::uint32_t kGameId = 0x4E535455u;   // M25 bump — NSTU, wire-format change

// Player AABB half-extents (0.8 m wide, 2 m tall). Defined here so host
// and client agree; used by LagCompensator + splash damage.
constexpr Vec3 kPlayerHalfExtents = Vec3{0.4f, 1.0f, 0.4f};

// Client interpolation delay — clients render remote players from
// TimeHistory at (renderNow - kInterpDelaySec). Same value drives
// viewTimeSec when firing.
constexpr double kInterpDelaySec = 0.10;

// Game tags. Tag 1 is HelloMsg (engine-owned). 254/255 are Ping/Pong
// (engine-owned). Game tags must live in [2, 253].

// Client -> Host: input tick. M19 — world-space velocity (m/s), NOT delta.
// M25 — adds `yaw` so the host can broadcast each peer's look direction
// for skinned-character facing.
struct PlayerInputMsg {
    static constexpr std::uint8_t kTag = 2;
    std::uint32_t inputId;
    float vx;             // world-space x velocity (m/s)
    float vy;             // reserved (unused in M19; always 0 on the wire)
    float vz;             // world-space z velocity (m/s)
    float yaw;            // M25 — look yaw (radians) for remote facing
    std::uint8_t jump;    // 0 = none, 1 = pressed this tick
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
    float vx, vy, vz;        // M19 — velocity for reconciliation
    std::uint8_t grounded;   // M19 — ground state
    float yaw;               // M25 — look yaw (radians) for remote facing
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

// Host -> Clients: broadcast when a player dies (HP -> 0). Carries the
// death foot position + a pre-computed impulse vector for the ragdoll
// torso. Each peer spawns its own ragdoll on receipt; no per-tick
// bone-transform sync (ragdolls are cosmetic-only, 2s lifetime).
struct DeathMsg {
    static constexpr std::uint8_t kTag = 11;
    std::uint32_t victimPeerId;
    float x, y, z;
    float impulseX, impulseY, impulseZ;
};

}  // namespace iron::netshooter
