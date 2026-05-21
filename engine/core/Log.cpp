#include "core/Log.h"

#include <cstdarg>
#include <cstdio>

namespace iron {

namespace {
void vlog(std::FILE* out, const char* tag, const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    std::fprintf(out, "[%s] ", tag);
    std::vfprintf(out, fmt, copy);
    std::fputc('\n', out);
    va_end(copy);
}
} // namespace

void Log::info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(stdout, "INFO", fmt, args);
    va_end(args);
}

void Log::warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(stderr, "WARN", fmt, args);
    va_end(args);
}

void Log::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(stderr, "ERROR", fmt, args);
    va_end(args);
}

} // namespace iron
