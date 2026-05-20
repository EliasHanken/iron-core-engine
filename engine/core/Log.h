#pragma once

#include <sal.h>

namespace iron {

// Minimal logging facade. Levels print to stdout (info) / stderr (warn, error)
// with a level tag. Swappable later without touching call sites.
class Log {
public:
    static void info(_Printf_format_string_ const char* fmt, ...);
    static void warn(_Printf_format_string_ const char* fmt, ...);
    static void error(_Printf_format_string_ const char* fmt, ...);
};

} // namespace iron
