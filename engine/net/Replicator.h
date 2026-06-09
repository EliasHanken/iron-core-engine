#pragma once

#include "core/Log.h"
#include "net/ByteStream.h"
#include "net/MessageRegistry.h"
#include "net/NetTransport.h"
#include "net/PeerManager.h"

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>

namespace iron {

// Game-assigned, stable, non-zero id for a replicated object.
using ReplicationId = std::uint32_t;

// Authoritative whole-object replication + client→server commands on top of
// PeerManager + MessageRegistry. The host owns authoritative objects, mutates
// them, marks them dirty, and flush()es (broadcast). Clients hold replicas that
// update on sync and fire an onReplicated (RepNotify) callback, and submit
// commands the host validates. All replication traffic rides one reserved tag
// (250), sub-dispatched internally.
//
// Lifetime: the referenced PeerManager and MessageRegistry MUST outlive the
// Replicator (it registers a raw handler capturing `this`; MessageRegistry has
// no unregister API).
//
// Reserved-tag convention: tag 1 = Hello (PeerManager), 250 = replication
// (Replicator), 254/255 = ping/pong (PeerManager). Games use tags 2–249.
class Replicator {
public:
    Replicator(PeerManager& peers, MessageRegistry& registry);
    ~Replicator();

    Replicator(const Replicator&) = delete;
    Replicator& operator=(const Replicator&) = delete;

    // Register a replicated object. Host: `obj` is the authoritative instance
    // (read on sync). Client: `obj` is the local replica (written on sync) and
    // `onReplicated` fires after each applied sync. Same call on both sides.
    template <typename T>
    void replicate(ReplicationId id, T* obj, std::function<void()> onReplicated = {}) {
        RepObject ro;
        ro.serialize   = [obj](ByteWriter& w) { serialize(w, *obj); };
        ro.deserialize = [obj](ByteReader& r) { deserialize(r, *obj); };
        ro.onReplicated = std::move(onReplicated);
        ro.dirty = false;
        objects_[id] = std::move(ro);
    }

    // Host: mark a replicated object changed; broadcast on the next flush().
    void markDirty(ReplicationId id);

    // Host: serialize + broadcast every dirty object (coalesced), clear dirty.
    // No-op on clients. Call once per tick AFTER peers.poll().
    void flush();

    // Host: send full current state of every replicated object to a freshly
    // joined peer. Call from the game's PeerManager onPeerJoined (peer != 0).
    void onPeerJoined(std::uint32_t peerId);

    // Host: register a validator/handler for a command type. The handler runs
    // only on the host, with the sender's peerId.
    template <typename Cmd>
    void onCommand(std::function<void(std::uint32_t, const Cmd&)> fn) {
        commandHandlers_[Cmd::kCmdId] =
            [fn = std::move(fn)](std::uint32_t fromPeer, ByteReader& r) {
                Cmd cmd{};
                deserialize(r, cmd);
                if (!r.failed()) fn(fromPeer, cmd);
            };
    }

    // Client: send a command to the host (reliable). No-op (warns) on the host.
    template <typename Cmd>
    void submitRequest(const Cmd& cmd) {
        if (peers_.isHost()) {
            Log::warn("Replicator: submitRequest called on host; ignored");
            return;
        }
        ByteWriter w;
        w.u8(kSubCommand);
        w.u32(Cmd::kCmdId);
        serialize(w, cmd);
        registry_.sendRaw(peers_.connectionFor(0), kReplicationTag,
                          std::span<const std::byte>(w.data().data(), w.data().size()),
                          SendReliability::Reliable);
    }

private:
    static constexpr std::uint8_t kReplicationTag = 250;
    static constexpr std::uint8_t kSubSync = 1;
    static constexpr std::uint8_t kSubCommand = 2;

    struct RepObject {
        std::function<void(ByteWriter&)> serialize;
        std::function<void(ByteReader&)> deserialize;
        std::function<void()> onReplicated;
        bool dirty = false;
    };

    void onPacket(ConnectionId conn, std::span<const std::byte> payload);
    void sendSyncTo(std::uint32_t peerId, ReplicationId id, RepObject& obj);

    PeerManager& peers_;
    MessageRegistry& registry_;
    std::unordered_map<ReplicationId, RepObject> objects_;
    std::unordered_map<std::uint32_t,
        std::function<void(std::uint32_t, ByteReader&)>> commandHandlers_;
};

}  // namespace iron
