#include "world/ComponentRegistry.h"

// ComponentRegistry is currently header-only (all methods are templates or
// inline). This TU exists so the build has a stable object file and to host
// non-inline additions later.
namespace iron {}  // namespace iron
