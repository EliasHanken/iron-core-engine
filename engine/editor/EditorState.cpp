#include "editor/EditorState.h"

// Everything in EditorState is inline in the header for now. This .cpp
// exists so CMake has a translation unit to compile for the ironcore_editor
// library — the link step needs at least one symbol from the file. Adding
// a touched-but-unused namespace-scope variable below keeps the TU
// non-empty without exposing anything.
namespace iron {
namespace { [[maybe_unused]] constexpr int kEditorStateTuMarker = 0; }
}  // namespace iron
