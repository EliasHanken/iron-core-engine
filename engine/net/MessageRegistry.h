#pragma once

#include "core/Log.h"
#include "net/NetTransport.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <type_traits>
#include <unordered_map>

namespace iron {

// Typed message dispatch on top of NetTransport. Wraps a single
// transport instance; installs its own onMessage handler at
// construction. Game code must NOT call transport->setOnMessage()
// after wrapping it in a MessageRegistry.
//
// Wire format: [u8 tag][raw memcpy of msg]
//
// Message type requirements (enforced by static_assert):
//   - std::is_trivially_copyable_v<Msg>
//   - Msg::kTag is a `static constexpr std::uint8_t`
//   - 1 + sizeof(Msg) <= kMaxPayloadBytes
//
// Tag 0 is reserved (treated as "invalid"). Tag namespace is per
// registry; different games can reuse tag values without conflict.
class MessageRegistry {
public:
    // Conservative MTU-ish ceiling so a single message can't fragment
    // unreliable delivery on a normal LAN.
    static constexpr std::size_t kMaxPayloadBytes = 1200;

    explicit MessageRegistry(NetTransport* transport);
    ~MessageRegistry();

    // Non-copyable, non-movable: holds a transport pointer and the
    // transport holds our dispatch lambda — copying would alias state.
    MessageRegistry(const MessageRegistry&) = delete;
    MessageRegistry& operator=(const MessageRegistry&) = delete;

    template <typename Msg>
    void registerHandler(std::function<void(ConnectionId, const Msg&)> fn) {
        static_assert(std::is_trivially_copyable_v<Msg>,
                      "MessageRegistry: Msg must be trivially copyable (POD)");
        static_assert(sizeof(typename std::remove_cv_t<decltype(Msg::kTag)>) == 1,
                      "MessageRegistry: Msg::kTag must be a uint8_t");
        static_assert(Msg::kTag != 0,
                      "MessageRegistry: tag 0 is reserved");
        static_assert(1 + sizeof(Msg) <= kMaxPayloadBytes,
                      "MessageRegistry: message too large");

        handlers_[Msg::kTag] = [fn = std::move(fn)](
                ConnectionId conn, std::span<const std::byte> payload) {
            if (payload.size() != sizeof(Msg)) {
                Log::warn("MessageRegistry: dropped Msg tag=%u, payload size %zu "
                          "!= sizeof(Msg) %zu",
                          static_cast<unsigned>(Msg::kTag),
                          payload.size(), sizeof(Msg));
                return;
            }
            Msg msg;
            std::memcpy(&msg, payload.data(), sizeof(Msg));
            fn(conn, msg);
        };
    }

    template <typename Msg>
    bool send(ConnectionId conn, const Msg& msg, SendReliability reliability) {
        static_assert(std::is_trivially_copyable_v<Msg>,
                      "MessageRegistry: Msg must be trivially copyable (POD)");
        static_assert(sizeof(typename std::remove_cv_t<decltype(Msg::kTag)>) == 1,
                      "MessageRegistry: Msg::kTag must be a uint8_t");
        static_assert(Msg::kTag != 0,
                      "MessageRegistry: tag 0 is reserved");
        static_assert(1 + sizeof(Msg) <= kMaxPayloadBytes,
                      "MessageRegistry: message too large");

        if (conn == kInvalidConnection) return false;
        std::byte buf[1 + sizeof(Msg)];
        buf[0] = std::byte{Msg::kTag};
        std::memcpy(&buf[1], &msg, sizeof(Msg));
        return transport_->send(
            conn, std::span<const std::byte>(buf, sizeof(buf)), reliability);
    }

    template <typename Msg>
    void sendToAll(std::span<const ConnectionId> conns,
                   const Msg& msg, SendReliability reliability) {
        for (ConnectionId c : conns) {
            send<Msg>(c, msg, reliability);
        }
    }

    // --- raw variable-length channel ---
    // For payloads that aren't fixed-size POD (e.g. the Replicator's snapshots).
    // The handler receives the payload AFTER the tag byte. Wire format is the
    // same [u8 tag][payload...]; raw tags share the same tag namespace as POD
    // messages, so don't reuse a tag for both.
    void registerRawHandler(std::uint8_t tag,
                            std::function<void(ConnectionId, std::span<const std::byte>)> fn);
    bool sendRaw(ConnectionId conn, std::uint8_t tag,
                 std::span<const std::byte> payload, SendReliability reliability);

private:
    // Dispatch entry point — non-template so it can live in the .cpp.
    void dispatch(ConnectionId conn, std::span<const std::byte> bytes);

    NetTransport* transport_;
    std::unordered_map<std::uint8_t,
        std::function<void(ConnectionId, std::span<const std::byte>)>> handlers_;
};

} // namespace iron
