#include "util/FileWatcher.h"
#include "test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace iron;
namespace fs = std::filesystem;

namespace {

std::string writeTempFile(const std::string& name, const std::string& content) {
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
    f.close();
    return p.string();
}

// Force the mtime forward by a fixed amount so poll() reliably detects it
// regardless of filesystem timestamp resolution.
void bumpMtime(const std::string& path, int secondsForward) {
    const auto t = fs::last_write_time(path);
    fs::last_write_time(path, t + std::chrono::seconds(secondsForward));
}

}  // namespace

int main() {
    // NoFireOnRegister — watch() captures mtime, so the first poll() is quiet.
    {
        const std::string p = writeTempFile("ironfw_a.txt", "v1");
        FileWatcher w;
        int fired = 0;
        w.watch(p, [&](const std::string&) { ++fired; });
        w.poll();                       // mtime captured at watch(); no change yet
        CHECK(fired == 0);
        fs::remove(p);
    }

    // FiresOnChange — an advanced mtime fires exactly once; a second quiet
    // poll() must not re-fire.
    {
        const std::string p = writeTempFile("ironfw_b.txt", "v1");
        FileWatcher w;
        int fired = 0;
        std::string firedPath;
        w.watch(p, [&](const std::string& path) { ++fired; firedPath = path; });
        bumpMtime(p, 10);
        w.poll();
        CHECK(fired == 1);
        CHECK(firedPath == p);
        // Second poll with no further change must not re-fire.
        w.poll();
        CHECK(fired == 1);
        fs::remove(p);
    }

    // UnwatchStopsCallbacks — after unwatch(), a change does not fire and the
    // entry is gone.
    {
        const std::string p = writeTempFile("ironfw_c.txt", "v1");
        FileWatcher w;
        int fired = 0;
        w.watch(p, [&](const std::string&) { ++fired; });
        w.unwatch(p);
        bumpMtime(p, 10);
        w.poll();
        CHECK(fired == 0);
        CHECK(w.watchCount() == 0u);
        fs::remove(p);
    }

    // FiresWhenMissingFileAppears — a missing path fires on the missing->present
    // transition.
    {
        const fs::path p = fs::temp_directory_path() / "ironfw_d.txt";
        fs::remove(p);  // ensure absent
        FileWatcher w;
        int fired = 0;
        w.watch(p.string(), [&](const std::string&) { ++fired; });
        w.poll();                       // still missing; no fire
        CHECK(fired == 0);
        { std::ofstream f(p); f << "now exists"; }
        w.poll();                       // missing -> present transition
        CHECK(fired == 1);
        fs::remove(p);
    }

    // WatchCountReflectsRegistrations — watchCount tracks watch/unwatch.
    {
        FileWatcher w;
        CHECK(w.watchCount() == 0u);
        const std::string a = writeTempFile("ironfw_e1.txt", "x");
        const std::string b = writeTempFile("ironfw_e2.txt", "y");
        w.watch(a, [](const std::string&) {});
        w.watch(b, [](const std::string&) {});
        CHECK(w.watchCount() == 2u);
        w.unwatch(a);
        CHECK(w.watchCount() == 1u);
        fs::remove(a);
        fs::remove(b);
    }

    return iron_test_result();
}
