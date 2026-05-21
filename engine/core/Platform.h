#pragma once

#include <string>

namespace iron {

// Absolute path of the directory containing the running executable.
//
// Games ship assets next to their binary, so asset paths should be resolved
// against this — not the current working directory, which depends on where
// the program happened to be launched from. Returns "." as a last resort if
// the OS query fails.
std::string executableDir();

} // namespace iron
