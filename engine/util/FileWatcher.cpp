#include "util/FileWatcher.h"

#include <filesystem>
#include <system_error>

namespace iron {

namespace fs = std::filesystem;

namespace {

// Read the file's mtime as a stable integer (ticks since the clock epoch).
// Returns 0 if the file is missing or unreadable.
std::int64_t mtimeOf(const std::string& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

}  // namespace

void FileWatcher::watch(std::string path, ChangeCallback onChange) {
    Entry e;
    e.onChange  = std::move(onChange);
    e.lastMtime = mtimeOf(path);   // capture now so first poll() is quiet
    entries_[std::move(path)] = std::move(e);
}

void FileWatcher::unwatch(const std::string& path) {
    entries_.erase(path);
}

void FileWatcher::poll() {
    for (auto& [path, entry] : entries_) {
        const std::int64_t now = mtimeOf(path);
        // 0 means "still missing / unreadable" — skip, retry next poll. Only
        // fire when we have a real, newer mtime than what we recorded.
        if (now != 0 && now != entry.lastMtime) {
            entry.lastMtime = now;
            if (entry.onChange) entry.onChange(path);
        }
    }
}

}  // namespace iron
