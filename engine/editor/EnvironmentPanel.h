#pragma once

namespace iron {

struct SceneFile;

// Edits the scene's global lighting + environment: clear color, sun, fog, and
// the point-light list. Mutates the scene in place.
class EnvironmentPanel {
public:
    // Returns true if any field changed this frame.
    bool draw(SceneFile& scene);
};

}  // namespace iron
