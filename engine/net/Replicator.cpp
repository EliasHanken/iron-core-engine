#include "net/Replicator.h"

#include <vector>

namespace iron {

Replicator::Replicator(PeerManager& peers, MessageRegistry& registry)
    : peers_(peers), registry_(registry) {
    registry_.registerRawHandler(
        kReplicationTag,
        [this](ConnectionId c, std::span<const std::byte> p) { onPacket(c, p); });
}

Replicator::~Replicator() {
    registry_.unregisterHandler(kReplicationTag);
}

void Replicator::remove(ReplicationId id) {
    objects_.erase(id);
}

void Replicator::markDirty(ReplicationId id) {
    const auto it = objects_.find(id);
    if (it != objects_.end()) it->second.dirty = true;
}

void Replicator::sendSyncTo(std::uint32_t peerId, ReplicationId id, RepObject& obj) {
    const ConnectionId c = peers_.connectionFor(peerId);
    if (c == kInvalidConnection) return;
    ByteWriter w;
    w.u8(kSubSync);
    w.u32(id);
    obj.serialize(w);
    if (1 + w.data().size() > MessageRegistry::kMaxPayloadBytes) {
        Log::warn("Replicator: object %u serializes to %zu bytes, exceeds max; skipped",
                  static_cast<unsigned>(id), w.data().size());
        return;
    }
    registry_.sendRaw(c, kReplicationTag,
                      std::span<const std::byte>(w.data().data(), w.data().size()),
                      SendReliability::Reliable);
}

void Replicator::flush() {
    if (!peers_.isHost()) return;
    const std::vector<std::uint32_t> ids = peers_.peerIds();
    for (auto& [id, obj] : objects_) {
        if (!obj.dirty) continue;
        for (std::uint32_t pid : ids) sendSyncTo(pid, id, obj);
        obj.dirty = false;
    }
}

void Replicator::onPeerJoined(std::uint32_t peerId) {
    if (!peers_.isHost() || peerId == 0) return;
    for (auto& [id, obj] : objects_) sendSyncTo(peerId, id, obj);
}

void Replicator::onPacket(ConnectionId conn, std::span<const std::byte> payload) {
    ByteReader r(payload);
    const std::uint8_t sub = r.u8();
    if (r.failed()) return;

    if (sub == kSubSync) {
        // Only clients apply syncs. The host owns authoritative state and must
        // never deserialize a (possibly forged) sync from a client into its own
        // authoritative object — state changes on the host go through onCommand.
        if (peers_.isHost()) return;
        const ReplicationId id = r.u32();
        const auto it = objects_.find(id);
        if (it == objects_.end()) return;
        it->second.deserialize(r);
        if (!r.failed() && it->second.onReplicated) it->second.onReplicated();
    } else if (sub == kSubCommand) {
        if (!peers_.isHost()) return;              // commands handled by host only
        const std::uint32_t cmdId = r.u32();
        if (r.failed()) return;
        const auto it = commandHandlers_.find(cmdId);
        if (it == commandHandlers_.end()) return;
        const auto fromPeer = peers_.peerIdFor(conn);
        if (!fromPeer) return;
        it->second(*fromPeer, r);
    }
}

}  // namespace iron
