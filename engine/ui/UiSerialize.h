#pragma once

#include "ui/UiElement.h"

#include <nlohmann/json.hpp>

namespace iron {

// Serialize a widget tree to/from JSON. Enums are stored as ints; `texture` (a
// runtime handle) and `id` (re-assigned on load via uiAssignIds) are not
// persisted. fromJson is tolerant: missing keys fall back to defaults.
nlohmann::json uiToJson(const UiElement& e);
UiElement uiFromJson(const nlohmann::json& j);

}  // namespace iron
