#pragma once

namespace iron {

// Minimal logging facade. Levels print to stdout (info) / stderr (warn, error)
// with a level tag. Swappable later without touching call sites.
class Log {
public:
    static void info(const char* fmt, ...);
    static void warn(const char* fmt, ...);
    static void error(const char* fmt, ...);
};

} // namespace iron
