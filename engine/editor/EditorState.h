#pragma once

namespace iron {

// Two-state editor mode flag. Edit (default) = scene authoring, physics
// and runtime systems paused. Play = scene is simulated, runtime systems
// active. The flag itself is just bookkeeping — the host (sandbox) owns
// the snapshot/restore logic and gates physics/audio/scripting on
// isPlaying().
class EditorState {
public:
    enum class Mode { Edit, Play };

    Mode mode() const { return mode_; }
    bool isPlaying() const { return mode_ == Mode::Play; }

    void setMode(Mode m) { mode_ = m; }

private:
    Mode mode_ = Mode::Edit;
};

}  // namespace iron
