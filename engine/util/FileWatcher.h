#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace iron {

// Polls registered file paths for modification-time changes and fires a
// callback when one advances. Polling (not OS notifications) keeps this
// portable and dependency-free; at our scale (a few dozen files) the
// per-frame stat() cost is sub-millisecond. Dev-only convenience for
// asset hot-reload.
class FileWatcher {
public:
    using ChangeCallback = std::function<void(const std::string& path)>;

    // Register `path` for monitoring. Captures the current mtime so an
    // immediate poll() does NOT fire. Re-watching a path replaces its
    // callback. A path that doesn't exist yet records mtime 0 ("missing")
    // and fires once it appears.
    void watch(std::string path, ChangeCallback onChange);

    // Stop monitoring `path`. No-op if not watched.
    void unwatch(const std::string& path);

    // Poll all registered paths. For any whose mtime advanced (or that
    // newly appeared), fire its callback and store the new mtime. A path
    // that fails to stat is left unchanged and retried next poll.
    void poll();

    std::size_t watchCount() const { return entries_.size(); }

private:
    struct Entry {
        ChangeCallback onChange;
        std::int64_t   lastMtime = 0;   // 0 = missing/unknown
    };
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace iron
