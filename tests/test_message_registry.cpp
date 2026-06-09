#include "test_framework.h"
#include "net/MessageRegistry.h"
#include "net/NetTransport.h"
#include "net/backends/mock/MockTransport.h"

#include <cstring>
#include <string>
#include <vector>

using namespace iron;

namespace {

// Test message types. POD, kTag, distinct sizes for the wrong-size test.
struct Foo { static constexpr std::uint8_t kTag = 1; std::uint32_t value; };
struct Bar { static constexpr std::uint8_t kTag = 2; float a; float b; };
struct Baz { static constexpr std::uint8_t kTag = 3; std::uint8_t flag; };

}  // namespace

int main() {
    constexpr NetAddress kAddr{0x7F000001u, 5555};

    // Round-trip per type.
    {
        MockTransport srv, cli;
        MessageRegistry srvReg(&srv);
        MessageRegistry cliReg(&cli);

        Foo gotFoo{};
        Bar gotBar{};
        Baz gotBaz{};
        int fooCount = 0, barCount = 0, bazCount = 0;

        srvReg.registerHandler<Foo>([&](ConnectionId, const Foo& m) {
            gotFoo = m; ++fooCount;
        });
        srvReg.registerHandler<Bar>([&](ConnectionId, const Bar& m) {
            gotBar = m; ++barCount;
        });
        srvReg.registerHandler<Baz>([&](ConnectionId, const Baz& m) {
            gotBaz = m; ++bazCount;
        });

        CHECK(srv.start()); CHECK(srv.listen(kAddr));
        CHECK(cli.start());
        const ConnectionId c = cli.connect(kAddr);
        CHECK(c != kInvalidConnection);
        srv.poll(); cli.poll();  // handshake

        CHECK(cliReg.send<Foo>(c, Foo{42}, SendReliability::Reliable));
        CHECK(cliReg.send<Bar>(c, Bar{1.5f, 2.5f}, SendReliability::Reliable));
        CHECK(cliReg.send<Baz>(c, Baz{7}, SendReliability::Reliable));
        srv.poll();

        CHECK(fooCount == 1); CHECK(gotFoo.value == 42);
        CHECK(barCount == 1); CHECK_NEAR(gotBar.a, 1.5f); CHECK_NEAR(gotBar.b, 2.5f);
        CHECK(bazCount == 1); CHECK(gotBaz.flag == 7);
    }

    // Wrong-size payload is silently dropped (no crash, no handler call).
    {
        MockTransport srv, cli;
        MessageRegistry srvReg(&srv);

        int fooCount = 0;
        srvReg.registerHandler<Foo>([&](ConnectionId, const Foo&) { ++fooCount; });

        CHECK(srv.start()); CHECK(srv.listen(kAddr));
        CHECK(cli.start());
        const ConnectionId c = cli.connect(kAddr);
        srv.poll(); cli.poll();

        // Send raw bytes [tag=Foo's tag][only 2 bytes of payload] via the
        // underlying transport, bypassing the registry's send<>.
        std::byte raw[3];
        raw[0] = std::byte{Foo::kTag};
        raw[1] = std::byte{0};
        raw[2] = std::byte{0};
        CHECK(cli.send(c, std::span<const std::byte>(raw, 3),
                       SendReliability::Reliable));
        srv.poll();

        CHECK(fooCount == 0);
    }

    // Unregistered tag silently dropped.
    {
        MockTransport srv, cli;
        MessageRegistry srvReg(&srv);

        // Only register Foo, never Bar.
        srvReg.registerHandler<Foo>([&](ConnectionId, const Foo&) {});

        CHECK(srv.start()); CHECK(srv.listen(kAddr));
        CHECK(cli.start());
        const ConnectionId c = cli.connect(kAddr);
        srv.poll(); cli.poll();

        // Build a Bar payload and send it via the raw transport with Bar's tag.
        const Bar payload{1.0f, 2.0f};
        std::byte raw[1 + sizeof(Bar)];
        raw[0] = std::byte{Bar::kTag};
        std::memcpy(&raw[1], &payload, sizeof(Bar));
        CHECK(cli.send(c, std::span<const std::byte>(raw, sizeof(raw)),
                       SendReliability::Reliable));
        srv.poll();
        // No crash — Bar handler not registered, message dropped.
    }

    // Multiple messages in one poll dispatched in order.
    {
        MockTransport srv, cli;
        MessageRegistry srvReg(&srv);
        MessageRegistry cliReg(&cli);

        std::vector<std::uint32_t> received;
        srvReg.registerHandler<Foo>([&](ConnectionId, const Foo& m) {
            received.push_back(m.value);
        });

        CHECK(srv.start()); CHECK(srv.listen(kAddr));
        CHECK(cli.start());
        const ConnectionId c = cli.connect(kAddr);
        srv.poll(); cli.poll();

        cliReg.send<Foo>(c, Foo{10}, SendReliability::Reliable);
        cliReg.send<Foo>(c, Foo{20}, SendReliability::Reliable);
        cliReg.send<Foo>(c, Foo{30}, SendReliability::Reliable);
        srv.poll();

        CHECK(received.size() == 3);
        CHECK(received[0] == 10);
        CHECK(received[1] == 20);
        CHECK(received[2] == 30);
    }

    // send<Msg> on kInvalidConnection returns false.
    {
        MockTransport t;
        MessageRegistry reg(&t);
        CHECK(t.start());
        CHECK(!reg.send<Foo>(kInvalidConnection, Foo{1}, SendReliability::Reliable));
    }

    // M64: raw variable-length channel round-trips an arbitrary payload under a tag.
    {
        MockTransport aT, bT;
        MessageRegistry aR(&aT), bR(&bT);
        // Wire them up directly (no PeerManager needed for this low-level test).
        aT.start(); bT.start();
        aT.listen(NetAddress{0x7F000001u, 7001});
        const ConnectionId aToB = bT.connect(NetAddress{0x7F000001u, 7001});
        for (int i = 0; i < 4; ++i) { aT.poll(); bT.poll(); }

        std::vector<std::byte> got;
        aR.registerRawHandler(200, [&](ConnectionId, std::span<const std::byte> p) {
            got.assign(p.begin(), p.end());
        });

        const std::byte payload[5] = {std::byte{1}, std::byte{2}, std::byte{3},
                                      std::byte{4}, std::byte{5}};
        CHECK(bR.sendRaw(aToB, 200, std::span<const std::byte>(payload, 5),
                         SendReliability::Reliable));
        aT.poll();
        CHECK(got.size() == 5u);
        CHECK(got[0] == std::byte{1});
        CHECK(got[4] == std::byte{5});
    }

    // M66: unregisterHandler detaches a handler; later messages with that tag
    // are dropped. Unregistering an absent tag is a harmless no-op.
    {
        MockTransport srv, cli;
        MessageRegistry srvReg(&srv);
        MessageRegistry cliReg(&cli);

        int fooCount = 0;
        srvReg.registerHandler<Foo>([&](ConnectionId, const Foo&) { ++fooCount; });

        CHECK(srv.start()); CHECK(srv.listen(kAddr));
        CHECK(cli.start());
        const ConnectionId c = cli.connect(kAddr);
        CHECK(c != kInvalidConnection);
        srv.poll(); cli.poll();

        CHECK(cliReg.send<Foo>(c, Foo{1}, SendReliability::Reliable));
        srv.poll();
        CHECK(fooCount == 1);

        srvReg.unregisterHandler(Foo::kTag);
        CHECK(cliReg.send<Foo>(c, Foo{2}, SendReliability::Reliable));
        srv.poll();
        CHECK(fooCount == 1);          // not incremented after unregister

        srvReg.unregisterHandler(99);  // absent tag → no crash
    }

    return iron_test_result();
}
