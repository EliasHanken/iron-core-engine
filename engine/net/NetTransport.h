#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace iron {

// Opaque connection handle. Stable for the lifetime of the connection
// within a single NetTransport instance. Never reused after close.
// kInvalidConnection (== 0) is the never-valid sentinel.
using ConnectionId = std::uint32_t;
constexpr ConnectionId kInvalidConnection = 0;

// Network endpoint. IPv4 only today; IPv6 / hostnames are future.
// `ipv4` is stored as a host-order 32-bit value (so 127.0.0.1 == 0x7F000001).
struct NetAddress {
    std::uint32_t ipv4 = 0x7F000001;  // 127.0.0.1 default
    std::uint16_t port = 0;

    friend bool operator==(const NetAddress&, const NetAddress&) = default;
};

enum class SendReliability {
    Reliable,    // retransmit + in-order
    Unreliable,  // best-effort, no ordering
};

// Transport-agnostic networking interface. One concrete implementation
// today (GnsTransport over GameNetworkingSockets); MockTransport exists
// for tests. Game code holds a NetTransport* / unique_ptr<NetTransport>
// and never includes GNS headers.
//
// Driving model:
//   1. Construct a concrete transport.
//   2. Install observer callbacks via the setOn... methods.
//   3. start() the transport.
//   4. listen(addr) and/or connect(addr) (a server may also act as a client).
//   5. Each game-loop tick, call poll(). The transport fires the observer
//      callbacks for any state changes / incoming messages.
//   6. send() to push bytes; close() to drop a connection; stop() to shut down.
class NetTransport {
public:
    virtual ~NetTransport();

    // --- lifecycle ---
    // Initialise the underlying transport. Returns false on failure.
    // Idempotent: start() after a successful start() is a no-op and returns true.
    virtual bool start() = 0;

    // Shut down. Closes every open connection without firing onConnectionClosed
    // for them (caller is shutting down on purpose). Any inbound messages
    // that arrived after the last poll() but before stop() are silently
    // dropped — drain via poll() first if you care about them. Idempotent.
    virtual void stop() = 0;

    // --- endpoints ---
    // Bind a listener at `addr`. Returns false on failure (port taken,
    // bad address, transport not started). New clients arrive via
    // onConnectionOpened.
    virtual bool listen(NetAddress addr) = 0;

    // Initiate a connection to `addr`. Returns a ConnectionId immediately.
    // The connection is NOT open yet — onConnectionOpened fires when the
    // handshake completes, or onConnectionClosed fires if it fails.
    // Returns kInvalidConnection on synchronous failure.
    virtual ConnectionId connect(NetAddress addr) = 0;

    // --- I/O ---
    // Send a byte buffer on `conn`. The buffer is copied internally —
    // caller can free immediately after return.
    // Returns false if `conn` is unknown or already closed (no callback
    // fires in that case).
    virtual bool send(ConnectionId conn,
                      std::span<const std::byte> bytes,
                      SendReliability reliability) = 0;

    // Close one connection. The remote side will see onConnectionClosed
    // on its next poll(). The local onConnectionClosed does NOT fire
    // for connections closed via this method (caller already knows).
    virtual void close(ConnectionId conn) = 0;

    // Drive the transport: dispatch status callbacks, drain inbound
    // message queues. Call once per game-loop tick on the same thread
    // that constructs the transport. Callbacks fire on the calling thread.
    virtual void poll() = 0;

    // --- observers ---
    // Set once after construction, before start() (later calls replace
    // the previous callback; older callbacks are discarded).
    using OnConnectionOpenedFn = std::function<void(ConnectionId)>;
    using OnConnectionClosedFn = std::function<void(ConnectionId,
                                                     const std::string& reason)>;
    using OnMessageFn          = std::function<void(ConnectionId,
                                                     std::span<const std::byte>)>;

    virtual void setOnConnectionOpened(OnConnectionOpenedFn) = 0;
    virtual void setOnConnectionClosed(OnConnectionClosedFn) = 0;
    virtual void setOnMessage(OnMessageFn) = 0;
};

} // namespace iron
