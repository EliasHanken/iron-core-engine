#pragma once

#include <string>

namespace iron {

struct SceneFile;

// Lists the scene's entities + a Save button + an add bar (add cube/plane/glTF,
// duplicate, delete). Pure UI: it mutates the selection index and returns the
// user's intent for this frame; the host performs the actual mutation/resolve
// (it owns the renderer + the resolved render data).
class SceneOutliner {
public:
    struct Result {
        enum class Action { None, AddCube, AddPlane, AddGltf, Delete, Duplicate };
        bool        saveClicked = false;
        Action      action = Action::None;
        std::string gltfPath;   // populated for AddGltf (from the path text field)
    };

    // `selectedIndex` is updated in place when the user clicks an entity row.
    Result draw(const SceneFile& scene, int& selectedIndex);

private:
    char gltfPathBuf_[256] = {};   // ImGui text buffer for the glTF path field
};

}  // namespace iron
