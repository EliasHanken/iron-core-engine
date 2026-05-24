#include "core/NetArgs.h"

#include "core/Log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace iron {

namespace {

// Parse "a.b.c.d" into a host-byte-order uint32_t. Returns false if any
// octet is missing, non-numeric, or out of [0, 255].
bool parseIPv4(std::string_view s, std::uint32_t& out) {
    std::uint32_t octets[4] = {0, 0, 0, 0};
    int idx = 0;
    std::size_t i = 0;
    while (i < s.size() && idx < 4) {
        std::uint32_t val = 0;
        bool any = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            val = val * 10 + static_cast<std::uint32_t>(s[i] - '0');
            if (val > 255) return false;
            ++i;
            any = true;
        }
        if (!any) return false;
        octets[idx++] = val;
        if (idx == 4) break;
        if (i >= s.size() || s[i] != '.') return false;
        ++i;
    }
    if (idx != 4 || i != s.size()) return false;
    out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return true;
}

}  // namespace

NetArgs parseNetArgs(int argc, char** argv) {
    NetArgs out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--connect" && i + 1 < argc) {
            std::uint32_t ip = 0;
            if (parseIPv4(argv[i + 1], ip)) {
                out.addr.ipv4 = ip;
                out.wantsConnect = true;
            } else {
                Log::warn("parseNetArgs: --connect value '%s' is not a valid IPv4",
                          argv[i + 1]);
            }
            ++i;
        } else if (arg == "--port" && i + 1 < argc) {
            char* end = nullptr;
            const long val = std::strtol(argv[i + 1], &end, 10);
            if (end != argv[i + 1] && *end == '\0' && val > 0 && val < 65536) {
                out.addr.port = static_cast<std::uint16_t>(val);
            } else {
                Log::warn("parseNetArgs: --port value '%s' is not a valid port",
                          argv[i + 1]);
            }
            ++i;
        }
        // Unknown flags silently ignored.
    }
    return out;
}

} // namespace iron
