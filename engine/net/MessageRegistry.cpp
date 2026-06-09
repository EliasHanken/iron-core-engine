#include "net/MessageRegistry.h"

#include <vector>

namespace iron {

MessageRegistry::MessageRegistry(NetTransport* transport)
    : transport_(transport) {
    transport_->setOnMessage(
        [this](ConnectionId conn, std::span<const std::byte> bytes) {
            this->dispatch(conn, bytes);
        });
}

MessageRegistry::~MessageRegistry() {
    // Defensive: drop our dispatch lambda from the transport so it can
    // be reused (or destroyed) without dangling reference to this.
    if (transport_) {
        transport_->setOnMessage(NetTransport::OnMessageFn{});
    }
}

void MessageRegistry::registerRawHandler(
        std::uint8_t tag,
        std::function<void(ConnectionId, std::span<const std::byte>)> fn) {
    // The handlers_ map already stores exactly this signature; a raw handler is
    // just one without the POD-memcpy wrapper. dispatch() passes the post-tag
    // payload through unchanged.
    handlers_[tag] = std::move(fn);
}

bool MessageRegistry::sendRaw(ConnectionId conn, std::uint8_t tag,
                              std::span<const std::byte> payload,
                              SendReliability reliability) {
    if (conn == kInvalidConnection) return false;
    if (1 + payload.size() > kMaxPayloadBytes) {
        Log::warn("MessageRegistry: sendRaw tag=%u payload %zu bytes exceeds max %zu; dropped",
                  static_cast<unsigned>(tag), payload.size(), kMaxPayloadBytes);
        return false;
    }
    std::vector<std::byte> buf;
    buf.reserve(1 + payload.size());
    buf.push_back(std::byte{tag});
    buf.insert(buf.end(), payload.begin(), payload.end());
    return transport_->send(conn, std::span<const std::byte>(buf.data(), buf.size()), reliability);
}

void MessageRegistry::dispatch(ConnectionId conn,
                                std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        Log::warn("MessageRegistry: dropped empty payload");
        return;
    }
    const std::uint8_t tag = static_cast<std::uint8_t>(bytes[0]);
    if (tag == 0) {
        Log::warn("MessageRegistry: dropped message with reserved tag=0");
        return;
    }
    auto it = handlers_.find(tag);
    if (it == handlers_.end()) {
        Log::warn("MessageRegistry: no handler for tag=%u",
                  static_cast<unsigned>(tag));
        return;
    }
    it->second(conn, bytes.subspan(1));
}

} // namespace iron
