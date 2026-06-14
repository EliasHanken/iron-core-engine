#include "editor/PrefabBrowser.h"

#include <imgui.h>

namespace iron {

// Strip a directory path down to its file stem for display (no dir, no extension).
static std::string displayName(const std::string& path) {
    std::string s = path;
    const auto slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const auto dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return s;
}

PrefabBrowser::Result PrefabBrowser::draw(const std::vector<std::string>& prefabPaths,
                                          bool selectionValid) {
    Result result;
    ImGui::Begin("Prefabs");

    // --- Create from selection ---
    ImGui::BeginDisabled(!selectionValid);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##prefabname", nameBuf_, sizeof(nameBuf_));
    ImGui::SameLine();
    if (ImGui::Button("Create Prefab from Selection") && nameBuf_[0] != '\0') {
        result.action = Result::Action::CreateFromSelection;
        result.name   = nameBuf_;
    }
    ImGui::EndDisabled();
    if (!selectionValid)
        ImGui::TextDisabled("(select an entity to create a prefab)");

    ImGui::Separator();
    ImGui::TextUnformatted("Prefabs:");

    // --- Instantiate a listed prefab ---
    if (prefabPaths.empty()) {
        ImGui::TextDisabled("(none in assets/prefabs)");
    } else {
        for (std::size_t i = 0; i < prefabPaths.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(displayName(prefabPaths[i]).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Instantiate")) {
                result.action     = Result::Action::Instantiate;
                result.prefabPath = prefabPaths[i];
            }
            ImGui::PopID();
        }
    }

    ImGui::End();
    return result;
}

}  // namespace iron
