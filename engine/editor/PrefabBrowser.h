#pragma once

#include <string>
#include <vector>

namespace iron {

// Lists discovered .prefab files and lets the user (a) create a prefab from the
// current selection and (b) instantiate a listed prefab. Pure UI: it returns the
// user's intent for this frame; the host performs the actual extract/save/load/
// instantiate (it owns the scene + World). Mirrors SceneOutliner's contract.
class PrefabBrowser {
public:
    struct Result {
        enum class Action { None, CreateFromSelection, Instantiate };
        Action      action = Action::None;
        std::string name;         // for CreateFromSelection (no extension)
        std::string prefabPath;   // for Instantiate (full path of the clicked prefab)
    };

    // `prefabPaths` is the host-supplied list of .prefab file paths to show.
    // `selectionValid` enables the "Create Prefab from Selection" controls.
    Result draw(const std::vector<std::string>& prefabPaths, bool selectionValid);

private:
    char nameBuf_[128] = {};   // ImGui text buffer for the new prefab's name
};

}  // namespace iron
