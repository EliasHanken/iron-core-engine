#include "Protocol.h"

#include <cstring>

namespace iron::netcubes {

namespace {
constexpr std::size_t kHelloSize    = 5;
constexpr std::size_t kPositionSize = 17;

void appendBytes(std::vector<std::byte>& out, const void* src, std::size_t n) {
    const auto* p = static_cast<const std::byte*>(src);
    out.insert(out.end(), p, p + n);
}
}  // namespace

void writeHello(std::vector<std::byte>& out, HelloMsg msg) {
    out.clear();
    out.reserve(kHelloSize);
    out.push_back(std::byte{static_cast<std::uint8_t>(MsgTag::Hello)});
    appendBytes(out, &msg.peerId, sizeof(msg.peerId));
}

void writePosition(std::vector<std::byte>& out, PositionMsg msg) {
    out.clear();
    out.reserve(kPositionSize);
    out.push_back(std::byte{static_cast<std::uint8_t>(MsgTag::Position)});
    appendBytes(out, &msg.peerId, sizeof(msg.peerId));
    appendBytes(out, &msg.x,      sizeof(msg.x));
    appendBytes(out, &msg.y,      sizeof(msg.y));
    appendBytes(out, &msg.z,      sizeof(msg.z));
}

std::optional<ParsedMsg> parse(std::span<const std::byte> bytes) {
    if (bytes.empty()) return std::nullopt;
    const auto tagByte = static_cast<std::uint8_t>(bytes[0]);
    switch (tagByte) {
        case static_cast<std::uint8_t>(MsgTag::Hello): {
            if (bytes.size() != kHelloSize) return std::nullopt;
            ParsedMsg out{};
            out.tag = MsgTag::Hello;
            std::memcpy(&out.hello.peerId, bytes.data() + 1, sizeof(std::uint32_t));
            return out;
        }
        case static_cast<std::uint8_t>(MsgTag::Position): {
            if (bytes.size() != kPositionSize) return std::nullopt;
            ParsedMsg out{};
            out.tag = MsgTag::Position;
            std::size_t off = 1;
            std::memcpy(&out.position.peerId, bytes.data() + off, sizeof(std::uint32_t)); off += sizeof(std::uint32_t);
            std::memcpy(&out.position.x,      bytes.data() + off, sizeof(float));         off += sizeof(float);
            std::memcpy(&out.position.y,      bytes.data() + off, sizeof(float));         off += sizeof(float);
            std::memcpy(&out.position.z,      bytes.data() + off, sizeof(float));
            return out;
        }
        default:
            return std::nullopt;
    }
}

}  // namespace iron::netcubes
