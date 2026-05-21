#include "core/Platform.h"

#include "core/Log.h"

#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace iron {

std::string executableDir() {
    // Getting the path of the running executable is inherently OS-specific —
    // there is no portable C++ API for it. Each platform has its own query.
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        // 0 = the call failed; MAX_PATH = the path filled the whole buffer
        // and may be truncated. Either way the result is not trustworthy.
        Log::error("executableDir: GetModuleFileNameW failed or path too long");
        return ".";
    }
    const std::filesystem::path exe(buffer, buffer + length);
    return exe.parent_path().string();
#elif defined(__linux__)
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer));
    if (length <= 0) {
        Log::error("executableDir: readlink(/proc/self/exe) failed");
        return ".";
    }
    const std::filesystem::path exe(buffer, buffer + length);
    return exe.parent_path().string();
#else
    // macOS would use _NSGetExecutablePath from <mach-o/dyld.h>; not yet
    // implemented. Fall back to the working directory so the build still
    // links — assets simply have to be run from alongside the executable.
    Log::warn("executableDir: unsupported platform; using working directory");
    return ".";
#endif
}

} // namespace iron
