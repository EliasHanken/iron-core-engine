#pragma once

#include "net/NetTransport.h"

namespace iron {

// Result of parsing networking CLI flags.
//   wantsConnect == false → caller should call transport.listen(addr)
//   wantsConnect == true  → caller should call transport.connect(addr)
//
// `addr.ipv4` is host-byte-order (so 127.0.0.1 == 0x7F000001).
struct NetArgs {
    NetAddress addr{0x7F000001u, 30005};
    bool wantsConnect = false;
};

// Parses `--connect <ip>` and `--port <n>` from argv. Unknown args are
// silently ignored. Malformed values (e.g. `--port abc`) leave the
// corresponding field at its default with a Log::warn for visibility.
// Returns sensible defaults if neither flag is present.
NetArgs parseNetArgs(int argc, char** argv);

} // namespace iron
