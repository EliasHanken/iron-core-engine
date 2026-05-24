#include "test_framework.h"
#include "core/NetArgs.h"

#include <cstring>
#include <string>
#include <vector>

using namespace iron;

namespace {

// Helper: build a char* argv from string literals; returns argc + argv.
struct Argv {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;
    int argc() const { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

Argv makeArgv(std::initializer_list<const char*> args) {
    Argv a;
    for (const char* s : args) a.storage.emplace_back(s);
    a.ptrs.reserve(a.storage.size());
    for (auto& s : a.storage) a.ptrs.push_back(s.data());
    return a;
}

}  // namespace

int main() {
    // No args → default listen on 127.0.0.1:30005
    {
        Argv a = makeArgv({"net-tag.exe"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == false);
        CHECK(out.addr.ipv4 == 0x7F000001u);
        CHECK(out.addr.port == 30005);
    }

    // --connect <ip> → wantsConnect, parsed IP, default port
    {
        Argv a = makeArgv({"net-tag.exe", "--connect", "192.168.1.5"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == true);
        CHECK(out.addr.ipv4 == ((192u << 24) | (168u << 16) | (1u << 8) | 5u));
        CHECK(out.addr.port == 30005);
    }

    // --port <n> → default IP, parsed port
    {
        Argv a = makeArgv({"net-tag.exe", "--port", "40000"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == false);
        CHECK(out.addr.ipv4 == 0x7F000001u);
        CHECK(out.addr.port == 40000);
    }

    // Both flags
    {
        Argv a = makeArgv({"net-tag.exe", "--connect", "10.0.0.1", "--port", "40000"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == true);
        CHECK(out.addr.ipv4 == ((10u << 24) | (0u << 16) | (0u << 8) | 1u));
        CHECK(out.addr.port == 40000);
    }

    // --connect missing value (last arg) → flag ignored, defaults preserved
    {
        Argv a = makeArgv({"net-tag.exe", "--connect"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == false);
        CHECK(out.addr.ipv4 == 0x7F000001u);
    }

    // --port malformed → port stays default
    {
        Argv a = makeArgv({"net-tag.exe", "--port", "abc"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.addr.port == 30005);
    }

    // --connect with malformed IP → wantsConnect stays false, IP stays default
    {
        Argv a = makeArgv({"net-tag.exe", "--connect", "not.an.ip"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.wantsConnect == false);
        CHECK(out.addr.ipv4 == 0x7F000001u);
    }

    // Unknown args silently ignored
    {
        Argv a = makeArgv({"net-tag.exe", "--unknown", "--port", "42"});
        NetArgs out = parseNetArgs(a.argc(), a.argv());
        CHECK(out.addr.port == 42);
    }

    return iron_test_result();
}
