#include "core/Platform.h"

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace iron {

std::string executableDir() {
    // Getting the path of the running executable is inherently OS-specific —
    // there is no portable C++ API for it. Each platform has its own query.
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return ".";  // query failed or the path was truncated
    }
    const std::filesystem::path exe(buffer, buffer + length);
#else
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer));
    if (length <= 0) {
        return ".";
    }
    const std::filesystem::path exe(buffer, buffer + length);
#endif
    return exe.parent_path().string();
}

} // namespace iron
