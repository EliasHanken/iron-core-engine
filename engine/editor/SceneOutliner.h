#pragma once

namespace iron {

struct SceneFile;

// Lists the scene's entities and a Save button. Pure UI: it mutates the
// selection index and reports whether Save was clicked; it does not own the
// scene or the file path (the host performs the save).
class SceneOutliner {
public:
    // Returns true if the user clicked "Save Scene" this frame.
    // `selectedIndex` is updated in place when the user clicks an entity.
    bool draw(const SceneFile& scene, int& selectedIndex);
};

}  // namespace iron
