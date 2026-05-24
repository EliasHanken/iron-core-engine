#include "test_framework.h"
#include "net/NetTransport.h"
#include "net/backends/mock/MockTransport.h"

#include <cstring>
#include <string>
#include <vector>

using namespace iron;

namespace {

std::span<const std::byte> asBytes(std::string_view sv) {
    return {reinterpret_cast<const std::byte*>(sv.data()), sv.size()};
}

std::string asString(std::span<const std::byte> b) {
    return std::string{reinterpret_cast<const char*>(b.data()), b.size()};
}

}  // namespace

int main() {
    constexpr NetAddress kAddr{0x7F000001, 1234};

    // Case 1: paired connect, both sides see onConnectionOpened.
    {
        MockTransport server, client;
        bool serverOpened = false, clientOpened = false;
        ConnectionId serverSawConn = kInvalidConnection;
        ConnectionId clientSawConn = kInvalidConnection;

        server.setOnConnectionOpened([&](ConnectionId c) {
            serverOpened = true;
            serverSawConn = c;
        });
        client.setOnConnectionOpened([&](ConnectionId c) {
            clientOpened = true;
            clientSawConn = c;
        });

        CHECK(server.start());
        CHECK(server.listen(kAddr));
        CHECK(client.start());

        const ConnectionId clientConn = client.connect(kAddr);
        CHECK(clientConn != kInvalidConnection);

        server.poll();
        client.poll();

        CHECK(serverOpened);
        CHECK(clientOpened);
        CHECK(serverSawConn != kInvalidConnection);
        CHECK(clientSawConn == clientConn);
    }

    // Case 2: send delivers via onMessage on the peer.
    {
        MockTransport server, client;
        std::string receivedOnServer;

        server.setOnMessage([&](ConnectionId, std::span<const std::byte> b) {
            receivedOnServer = asString(b);
        });
        CHECK(server.start());
        CHECK(server.listen(kAddr));
        CHECK(client.start());
        const ConnectionId clientConn = client.connect(kAddr);
        server.poll();
        client.poll();

        CHECK(client.send(clientConn, asBytes("hello"), SendReliability::Reliable));
        server.poll();

        CHECK(receivedOnServer == "hello");
    }

    // Case 3: reliable preserves in-order delivery for multiple sends.
    {
        MockTransport server, client;
        std::vector<std::string> received;
        server.setOnMessage([&](ConnectionId, std::span<const std::byte> b) {
            received.push_back(asString(b));
        });
        CHECK(server.start());
        CHECK(server.listen(kAddr));
        CHECK(client.start());
        const ConnectionId clientConn = client.connect(kAddr);
        server.poll();
        client.poll();

        client.send(clientConn, asBytes("one"),   SendReliability::Reliable);
        client.send(clientConn, asBytes("two"),   SendReliability::Reliable);
        client.send(clientConn, asBytes("three"), SendReliability::Reliable);
        server.poll();

        CHECK(received.size() == 3);
        CHECK(received[0] == "one");
        CHECK(received[1] == "two");
        CHECK(received[2] == "three");
    }

    // Case 4: close() fires onConnectionClosed on peer (NOT on caller).
    {
        MockTransport server, client;
        bool serverClosed = false;
        bool clientClosed = false;
        server.setOnConnectionClosed([&](ConnectionId, const std::string&) {
            serverClosed = true;
        });
        client.setOnConnectionClosed([&](ConnectionId, const std::string&) {
            clientClosed = true;
        });
        CHECK(server.start());
        CHECK(server.listen(kAddr));
        CHECK(client.start());
        const ConnectionId clientConn = client.connect(kAddr);
        server.poll();
        client.poll();

        client.close(clientConn);
        server.poll();
        client.poll();

        CHECK(serverClosed);
        CHECK(!clientClosed);  // local caller asked for the close
    }

    // Case 5: send on closed connection returns false; no callback, no crash.
    {
        MockTransport server, client;
        int messageCount = 0;
        server.setOnMessage([&](ConnectionId, std::span<const std::byte>) {
            ++messageCount;
        });
        CHECK(server.start());
        CHECK(server.listen(kAddr));
        CHECK(client.start());
        const ConnectionId clientConn = client.connect(kAddr);
        server.poll();
        client.poll();

        client.close(clientConn);

        CHECK(!client.send(clientConn, asBytes("x"), SendReliability::Reliable));
        server.poll();
        CHECK(messageCount == 0);
    }

    // Case 6: send on invalid id returns false.
    {
        MockTransport t;
        CHECK(t.start());
        CHECK(!t.send(kInvalidConnection, asBytes("x"), SendReliability::Reliable));
        CHECK(!t.send(99999, asBytes("x"), SendReliability::Reliable));
    }

    return iron_test_result();
}
