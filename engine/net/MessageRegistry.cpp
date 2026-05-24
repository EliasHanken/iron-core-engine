#include "net/MessageRegistry.h"

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
