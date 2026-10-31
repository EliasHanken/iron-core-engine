#pragma once

#include "editor/UndoHistory.h"
#include "forge/Forge.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace iron {

// The Forge editing panel: plain stacks of blocks with sliders. ImGui stays inside
// the .cpp, like every editor module.
class ForgePanel {
public:
    struct Status {
        std::string fileName;
        std::uint32_t particles = 0;
        float frameMs = 0.0f;
    };

    enum class Action { None, Save, Restart };

    // Draws everything. `document` changes as the user edits, and `changed` is set
    // when it does. Returns a request for the host to act on.
    Action draw(forge::Document& document, const std::vector<forge::Issue>& issues,
                const Status& status, bool& changed);

    // Width in pixels, so that the host can centre its picture in the rest.
    float width() const { return width_; }

    // A line of text over the picture, for scripted demos.
    void setCaption(std::string caption) { caption_ = std::move(caption); }

    // Forgets undo history, for when a whole new document arrives.
    void resetHistory() { history_.clear(); }

    void setVisibleLayer(int layer) { selectedLayer_ = layer; }

private:
    void drawStack(forge::Document& document, std::size_t layerIndex, forge::Stack stack,
                   bool& changed);
    void drawParam(forge::Document& document, const std::string& path, const forge::ParamDesc& param,
                   std::map<std::string, forge::Value>& values, bool& changed, int depth);
    void drawShapes(forge::Document& document, bool& changed);
    void trackUndo(const forge::Document& document);

    float width_ = 410.0f;
    int   selectedLayer_ = 0;
    std::string caption_;
    std::string addFilter_;

    UndoHistory history_;
    std::string editStart_;
};

}  // namespace iron
