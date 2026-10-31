#include "editor/ForgePanel.h"

#include "forge/ForgeEdit.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace iron {

namespace {

using forge::Stack;

ImVec4 stackColor(Stack stack) {
    switch (stack) {
        case Stack::Spawn: return ImVec4(0.36f, 0.78f, 0.56f, 1.0f);
        case Stack::Move: return ImVec4(0.40f, 0.62f, 1.0f, 1.0f);
        case Stack::Look: return ImVec4(1.0f, 0.62f, 0.30f, 1.0f);
        default: return ImVec4(0.86f, 0.50f, 0.95f, 1.0f);
    }
}

const char* stackTitle(Stack stack) {
    switch (stack) {
        case Stack::Spawn: return "SPAWN   where they start";
        case Stack::Move: return "MOVE   what happens to them";
        case Stack::Look: return "LOOK   how they're drawn";
        default: return "SURFACE   what it's made of";
    }
}

bool containsIgnoringCase(const std::string& text, const std::string& query) {
    const auto found = std::search(text.begin(), text.end(), query.begin(), query.end(),
                                   [](char a, char b) {
                                       return std::tolower(static_cast<unsigned char>(a)) ==
                                              std::tolower(static_cast<unsigned char>(b));
                                   });
    return query.empty() || found != text.end();
}

}  // namespace

void ForgePanel::trackUndo(const forge::Document& document) {
    if (ImGui::IsItemActivated()) editStart_ = forge::documentToJson(document).dump();
    if (ImGui::IsItemDeactivatedAfterEdit() && !editStart_.empty()) {
        history_.commit(editStart_);
        editStart_.clear();
    }
}

void ForgePanel::drawParam(forge::Document& document, const std::string& path,
                           const forge::ParamDesc& param,
                           std::map<std::string, forge::Value>& values, bool& changed, int depth) {
    ImGui::PushID(param.name.c_str());

    const std::string driver = forge::driverOf(document, path);
    const auto it = values.find(param.name);
    forge::Value value = it != values.end() ? it->second : param.defaultValue;

    const float buttonWidth = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f - buttonWidth);

    bool edited = false;
    if (!driver.empty()) {
        ImGui::BeginDisabled();
        float shown = 0.0f;
        ImGui::SliderFloat("##driven", &shown, 0.0f, 1.0f, "driven");
        ImGui::EndDisabled();
    } else if (param.type == forge::ValueType::Float) {
        edited = ImGui::SliderFloat("##value", &value.x, param.min, param.max, "%.3f");
        trackUndo(document);
    } else if (param.type == forge::ValueType::Color) {
        float rgb[3] = {value.x, value.y, value.z};
        edited = ImGui::ColorEdit3("##value", rgb, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
        trackUndo(document);
        value = {rgb[0], rgb[1], rgb[2]};
    } else {
        float xyz[3] = {value.x, value.y, value.z};
        edited = ImGui::DragFloat3("##value", xyz, 0.02f, param.min, param.max, "%.2f");
        trackUndo(document);
        value = {xyz[0], xyz[1], xyz[2]};
    }
    if (edited) {
        values[param.name] = value;
        changed = true;
    }

    ImGui::SameLine();
    ImGui::TextUnformatted(param.name.c_str());
    if (!param.description.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", param.description.c_str());
    }

    // Any number can be driven by a node, without leaving the stack.
    if (param.type == forge::ValueType::Float && depth == 0) {
        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth);
        if (ImGui::Button(driver.empty() ? "~" : "x", ImVec2(buttonWidth, 0.0f))) {
            if (driver.empty()) {
                ImGui::OpenPopup("drive");
            } else {
                history_.commit(forge::documentToJson(document).dump());
                forge::undriveValue(document, path);
                changed = true;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(driver.empty() ? "Drive this with a node" : "Stop driving");
        if (ImGui::BeginPopup("drive")) {
            for (const forge::NodeDesc& node : forge::nodes()) {
                if (node.output != forge::ValueType::Float) continue;
                if (ImGui::Selectable(node.name.c_str())) {
                    history_.commit(forge::documentToJson(document).dump());
                    forge::driveValue(document, path, node.name);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", node.summary.c_str());
            }
            ImGui::EndPopup();
        }
    }

    if (!driver.empty() && document.nodes.count(driver) != 0) {
        forge::NodeInstance& node = document.nodes.at(driver);
        if (const forge::NodeDesc* desc = forge::findNode(node.type)) {
            ImGui::Indent(14.0f);
            ImGui::TextDisabled("%s", node.type.c_str());
            for (const forge::ParamDesc& input : desc->inputs) {
                drawParam(document, driver + "." + input.name, input, node.values, changed, depth + 1);
            }
            ImGui::Unindent(14.0f);
        }
    }

    ImGui::PopID();
}

void ForgePanel::drawStack(forge::Document& document, std::size_t layerIndex, Stack stack,
                           bool& changed) {
    auto& blocks = stack == Stack::Surface ? document.surface
                                           : forge::stackOf(document.layers[layerIndex], stack);
    const ImVec4 color = stackColor(stack);

    ImGui::PushID(static_cast<int>(stack));
    ImGui::Spacing();
    ImGui::TextColored(color, "%s", stackTitle(stack));
    ImGui::Separator();

    int removeIndex = -1;
    int moveIndex = -1;
    int moveOffset = 0;

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        forge::BlockInstance& block = blocks[i];
        const forge::BlockDesc* desc = forge::findBlock(block.block);
        if (!desc) continue;

        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(color.x * 0.22f, color.y * 0.22f, color.z * 0.22f, 1.0f));
        const float buttons = ImGui::GetFrameHeight() * 3.0f + 16.0f;
        const bool open = ImGui::CollapsingHeader(block.block.c_str(), ImGuiTreeNodeFlags_DefaultOpen |
                                                  ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc->summary.c_str());

        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttons);
        if (ImGui::SmallButton("^")) { moveIndex = static_cast<int>(i); moveOffset = -1; }
        ImGui::SameLine();
        if (ImGui::SmallButton("v")) { moveIndex = static_cast<int>(i); moveOffset = 1; }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeIndex = static_cast<int>(i);

        if (open) {
            if (desc->needsShape) {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
                if (ImGui::BeginCombo("shape", block.shape.c_str())) {
                    for (const auto& [name, shape] : document.shapes) {
                        if (ImGui::Selectable(name.c_str(), name == block.shape)) {
                            history_.commit(forge::documentToJson(document).dump());
                            block.shape = name;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            for (const forge::ParamDesc& param : desc->params) {
                drawParam(document, forge::valuePath(layerIndex, stack, i, param.name), param,
                          block.values, changed, 0);
            }
        }
        ImGui::PopID();
    }

    if (removeIndex >= 0) {
        history_.commit(forge::documentToJson(document).dump());
        forge::removeBlock(document, layerIndex, stack, static_cast<std::size_t>(removeIndex));
        changed = true;
    }
    if (moveIndex >= 0) {
        history_.commit(forge::documentToJson(document).dump());
        forge::moveBlock(document, layerIndex, stack, static_cast<std::size_t>(moveIndex), moveOffset);
        changed = true;
    }

    if (ImGui::Button("+ Add", ImVec2(-1.0f, 0.0f))) {
        addFilter_.clear();
        ImGui::OpenPopup("add");
    }
    if (ImGui::BeginPopup("add")) {
        char filter[64] = {};
        std::snprintf(filter, sizeof(filter), "%s", addFilter_.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::InputTextWithHint("##filter", "Search", filter, sizeof(filter))) addFilter_ = filter;
        for (const forge::BlockDesc& desc : forge::blocks()) {
            if (desc.stack != stack) continue;
            if (!containsIgnoringCase(desc.name + " " + desc.summary, addFilter_)) continue;
            if (ImGui::Selectable(desc.name.c_str())) {
                history_.commit(forge::documentToJson(document).dump());
                forge::Layer scratch;
                forge::addBlock(document, stack == Stack::Surface ? scratch : document.layers[layerIndex],
                                stack, desc.name);
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(150.0f);
            ImGui::TextDisabled("%s", desc.summary.c_str());
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void ForgePanel::drawShapes(forge::Document& document, bool& changed) {
    if (document.shapes.empty()) return;
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.86f, 1.0f), "SHAPES");
    ImGui::Separator();
    for (auto& [name, shape] : document.shapes) {
        ImGui::PushID(name.c_str());
        const forge::ShapeDesc* desc = forge::findShape(shape.type);
        const std::string label = name + "  (" + shape.type + ")";
        if (ImGui::CollapsingHeader(label.c_str()) && desc) {
            if (ImGui::BeginCombo("type", shape.type.c_str())) {
                for (const forge::ShapeDesc& option : forge::shapes()) {
                    if (ImGui::Selectable(option.name.c_str(), option.name == shape.type)) {
                        history_.commit(forge::documentToJson(document).dump());
                        shape.type = option.name;
                        shape.values.clear();
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            for (const forge::ParamDesc& param : desc->params) {
                drawParam(document, "shapes." + name + "." + param.name, param, shape.values, changed, 1);
            }
        }
        ImGui::PopID();
    }
}

ForgePanel::Action ForgePanel::draw(forge::Document& document, const std::vector<forge::Issue>& issues,
                                    const Status& status, bool& changed) {
    Action action = Action::None;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(ImVec2(width_, viewport->Size.y));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::Begin("Forge", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.30f, 1.0f), "FORGE");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status.fileName.c_str());

    if (ImGui::BeginCombo("##template", "New from...")) {
        for (const std::string& name : forge::templateNames()) {
            if (ImGui::Selectable(name.c_str())) {
                history_.commit(forge::documentToJson(document).dump());
                document = forge::makeTemplate(name);
                selectedLayer_ = 0;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) action = Action::Save;
    ImGui::SameLine();
    if (ImGui::Button("Restart")) action = Action::Restart;
    ImGui::SameLine();
    ImGui::BeginDisabled(!history_.canUndo());
    const bool undoPressed = ImGui::Button("Undo");
    ImGui::EndDisabled();

    const bool undoKey = ImGui::IsKeyPressed(ImGuiKey_Z) && ImGui::GetIO().KeyCtrl &&
                         !ImGui::GetIO().WantTextInput;
    const bool redoKey = ImGui::IsKeyPressed(ImGuiKey_Y) && ImGui::GetIO().KeyCtrl &&
                         !ImGui::GetIO().WantTextInput;
    const auto restore = [&](const std::optional<std::string>& snapshot) {
        if (!snapshot) return;
        std::vector<forge::Issue> ignored;
        document = forge::parseDocument(nlohmann::json::parse(*snapshot), ignored);
        changed = true;
    };
    if (undoPressed || undoKey) restore(history_.undo(forge::documentToJson(document).dump()));
    if (redoKey) restore(history_.redo(forge::documentToJson(document).dump()));

    ImGui::Separator();

    if (!issues.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        for (const forge::Issue& issue : issues) {
            ImGui::TextWrapped("%s %s", issue.message.c_str(), issue.fix.c_str());
        }
        ImGui::PopStyleColor();
        ImGui::TextDisabled("The last working version is still playing.");
        ImGui::Separator();
    }

    ImGui::BeginChild("stacks");
    if (document.target == "surface") {
        drawStack(document, 0, Stack::Surface, changed);
    } else if (!document.layers.empty()) {
        selectedLayer_ = std::clamp(selectedLayer_, 0, static_cast<int>(document.layers.size()) - 1);
        if (ImGui::BeginTabBar("layers")) {
            for (std::size_t i = 0; i < document.layers.size(); ++i) {
                const std::string label = document.layers[i].name + "##" + std::to_string(i);
                const ImGuiTabItemFlags flags = static_cast<int>(i) == selectedLayer_
                                                    ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(label.c_str(), nullptr, flags)) ImGui::EndTabItem();
                if (ImGui::IsItemClicked()) selectedLayer_ = static_cast<int>(i);
            }
            if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing)) {
                history_.commit(forge::documentToJson(document).dump());
                forge::Layer layer;
                layer.name = "layer " + std::to_string(document.layers.size() + 1);
                forge::addBlock(document, layer, Stack::Spawn, "Sphere");
                forge::addBlock(document, layer, Stack::Look, "Glow");
                document.layers.push_back(std::move(layer));
                selectedLayer_ = static_cast<int>(document.layers.size()) - 1;
                changed = true;
            }
            ImGui::EndTabBar();
        }

        forge::Layer& layer = document.layers[static_cast<std::size_t>(selectedLayer_)];
        int count = static_cast<int>(layer.count);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
        if (ImGui::SliderInt("particles", &count, 1000, 1500000, "%d", ImGuiSliderFlags_Logarithmic)) {
            layer.count = static_cast<std::uint32_t>(count);
        }
        // Resizing rebuilds the layer, so it waits for the slider's release.
        trackUndo(document);
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;

        const auto index = static_cast<std::size_t>(selectedLayer_);
        drawStack(document, index, Stack::Spawn, changed);
        drawStack(document, index, Stack::Move, changed);
        drawStack(document, index, Stack::Look, changed);
    }
    drawShapes(document, changed);
    ImGui::EndChild();
    ImGui::End();

    // Stats, top right of the picture.
    ImDrawList* overlay = ImGui::GetForegroundDrawList();
    char line[96];
    std::snprintf(line, sizeof(line), "%u particles   %.1f ms", status.particles, status.frameMs);
    const ImVec2 size = ImGui::CalcTextSize(line);
    overlay->AddText(ImVec2(viewport->Pos.x + viewport->Size.x - size.x - 18.0f, viewport->Pos.y + 12.0f),
                     IM_COL32(210, 210, 225, 170), line);

    if (!caption_.empty()) {
        const float scale = 1.7f;
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * scale;
        const ImVec2 textSize = font->CalcTextSizeA(fontSize, 10000.0f, 0.0f, caption_.c_str());
        const float centerX = width_ + (viewport->Size.x - width_) * 0.5f;
        const ImVec2 at(centerX - textSize.x * 0.5f, viewport->Pos.y + viewport->Size.y - 84.0f);
        overlay->AddRectFilled(ImVec2(at.x - 18.0f, at.y - 9.0f),
                               ImVec2(at.x + textSize.x + 18.0f, at.y + textSize.y + 9.0f),
                               IM_COL32(8, 8, 12, 165), 6.0f);
        overlay->AddText(font, fontSize, at, IM_COL32(245, 240, 232, 255), caption_.c_str());
    }

    return action;
}

}  // namespace iron
