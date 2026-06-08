#include "ui/UiSerialize.h"

namespace iron {

namespace {

nlohmann::json v2(Vec2 v) { return nlohmann::json{{"x", v.x}, {"y", v.y}}; }
nlohmann::json v4(Vec4 v) { return nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}, {"w", v.w}}; }

Vec2 readV2(const nlohmann::json& j, Vec2 def) {
    if (!j.is_object()) return def;
    return Vec2{j.value("x", def.x), j.value("y", def.y)};
}
Vec4 readV4(const nlohmann::json& j, Vec4 def) {
    if (!j.is_object()) return def;
    return Vec4{j.value("x", def.x), j.value("y", def.y), j.value("z", def.z), j.value("w", def.w)};
}

}  // namespace

nlohmann::json uiToJson(const UiElement& e) {
    nlohmann::json j;
    j["kind"]       = static_cast<int>(e.kind);
    j["anchor"]     = static_cast<int>(e.anchor);
    j["offset"]     = v2(e.offset);
    j["size"]       = v2(e.size);
    j["color"]      = v4(e.color);
    j["visible"]    = e.visible;
    j["stack"]      = static_cast<int>(e.stack);
    j["spacing"]    = e.spacing;
    j["text"]       = e.text;
    j["fontPx"]     = e.fontPx;
    j["value"]      = e.value;
    j["trackColor"] = v4(e.trackColor);
    j["actionId"]   = e.actionId;

    nlohmann::json kids = nlohmann::json::array();
    for (const UiElement& c : e.children) kids.push_back(uiToJson(c));
    j["children"] = std::move(kids);
    return j;
}

UiElement uiFromJson(const nlohmann::json& j) {
    UiElement e;
    e.kind       = static_cast<UiKind>(j.value("kind", 0));
    e.anchor     = static_cast<Anchor>(j.value("anchor", 0));
    e.offset     = readV2(j.contains("offset") ? j["offset"] : nlohmann::json{}, Vec2{0, 0});
    e.size       = readV2(j.contains("size") ? j["size"] : nlohmann::json{}, Vec2{0, 0});
    e.color      = readV4(j.contains("color") ? j["color"] : nlohmann::json{}, Vec4{1, 1, 1, 1});
    e.visible    = j.value("visible", true);
    e.stack      = static_cast<StackDir>(j.value("stack", 0));
    e.spacing    = j.value("spacing", 0.0f);
    e.text       = j.value("text", std::string());
    e.fontPx     = j.value("fontPx", 18.0f);
    e.value      = j.value("value", 0.0f);
    e.trackColor = readV4(j.contains("trackColor") ? j["trackColor"] : nlohmann::json{}, Vec4{0, 0, 0, 1});
    e.actionId   = j.value("actionId", 0u);

    if (j.contains("children") && j["children"].is_array())
        for (const auto& jc : j["children"]) e.children.push_back(uiFromJson(jc));
    return e;
}

}  // namespace iron
