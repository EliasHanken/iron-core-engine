// Iron Core Engine — networking smoke test (M8.1, on iron::NetTransport).
//
// Single process, single GnsTransport that both listens and connects on
// the same localhost UDP port. GNS loopback-connects the outgoing
// connection to the listen socket on the same interface, so no second
// process is needed.
//
// Flow:
//   transport.listen(...)             — server side
//   clientConn = transport.connect(...)  — client side
//   onConnectionOpened(clientConn)    — send PING
//   onMessage (server-accepted conn)  — reply PONG
//   onMessage (clientConn)            — verify PONG, set done=true
//
// This is the integration test for iron::GnsTransport — the unit test
// for the contract lives in tests/test_mock_net_transport.cpp.
//
// Note: Two GnsTransport instances per process are not supported because
// GameNetworkingSockets_Init/Kill are process-global. The single-transport
// variant proves the wrapper works end-to-end via real sockets.
//
// Failure modes (all exit code 1):
//   - GnsTransport::start fails
//   - listen/connect returns kInvalidConnection
//   - either side fails to open within 5 s
//   - PING or PONG mismatch
//   - any other 5 s wall-clock timeout

#include "core/Log.h"
#include "net/NetTransport.h"
#include "net/backends/gns/GnsTransport.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::span<const std::byte> asBytes(std::string_view sv) {
    return {reinterpret_cast<const std::byte*>(sv.data()), sv.size()};
}

std::string_view asStr(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

}  // namespace

int main() {
    iron::GnsTransport transport;

    bool done = false;
    bool failed = false;
    iron::ConnectionId clientConn = iron::kInvalidConnection;

    auto failWith = [&](const char* reason) {
        std::fprintf(stderr, "net-pingpong: %s\n", reason);
        failed = true;
    };

    // onConnectionOpened fires for both the client-initiated connection
    // and the server-accepted connection. We use clientConn to tell them apart.
    transport.setOnConnectionOpened([&](iron::ConnectionId c) {
        if (c == clientConn) {
            // Client side is now open — send PING.
            if (!transport.send(c, asBytes("PING"), iron::SendReliability::Reliable)) {
                failWith("client send PING failed");
            }
        }
        // Server-accepted connection: no action needed here; wait for message.
    });

    transport.setOnMessage([&](iron::ConnectionId c, std::span<const std::byte> b) {
        const auto msg = asStr(b);
        if (c == clientConn) {
            // Client received a reply.
            if (msg == "PONG") done = true;
            else failWith("client received non-PONG");
        } else {
            // Server received a message from the client.
            if (msg != "PING") { failWith("server received non-PING"); return; }
            if (!transport.send(c, asBytes("PONG"), iron::SendReliability::Reliable)) {
                failWith("server send PONG failed");
            }
        }
    });

    transport.setOnConnectionClosed([&](iron::ConnectionId, const std::string&) {
        if (!done) failWith("connection closed before exchange completed");
    });

    if (!transport.start()) { failWith("transport start failed"); return 1; }

    const iron::NetAddress addr{0x7F000001, 27015};
    if (!transport.listen(addr)) { failWith("transport.listen failed"); return 1; }
    clientConn = transport.connect(addr);
    if (clientConn == iron::kInvalidConnection) { failWith("transport.connect failed"); return 1; }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done && !failed && std::chrono::steady_clock::now() < deadline) {
        transport.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done && !failed) failWith("timeout waiting for ping-pong exchange");

    transport.stop();

    if (done && !failed) {
        std::printf("OK\n");
        return 0;
    }
    return 1;
}
