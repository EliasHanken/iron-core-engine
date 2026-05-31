#pragma once

#include "reflection/Reflection.h"

// ImGui-rendered Inspector helper driven by the iron::Reflection registry.
// Walks fieldsOf<T>(), draws one widget per field by dispatching on TypeId,
// returns true if any field changed this frame. The component's type name
// (typeName<T>()) is used as the section header (ImGui::SeparatorText).
//
// Widget rules per TypeId:
//   Bool         → Checkbox
//   Int32/UInt32 → InputInt (clamped if min/max set)
//   UInt8        → InputInt (clamped to [0, 255] regardless of meta)
//   Float        → SliderFloat if meta.slider, else DragFloat (speed from
//                  meta.dragSpeed or 0.05; clamped if min/max set)
//   String       → InputText (size grows as user types)
//   Vec3         → ColorEdit3 if meta.color, else DragFloat3 (speed from
//                  meta.dragSpeed or 0.05)
//   Quat         → euler-decomposed DragFloat3 (degrees); recompose on edit
//                  (speed = meta.dragSpeed or 0.5)
//   Enum         → Combo over enumValuesById(f.enumTypeId); unknown current
//                  value shows index 0 with a per-frame warning and does not
//                  write back until the user picks a value
//   OptionalEnum → "None" + Combo; None resets the optional, others assign
//
// ImGui include is kept private to ReflectionInspector.cpp — consumers of the
// header don't pull <imgui.h>.

namespace iron {

bool renderComponentByPtr(const Reflection& r,
                          std::string_view typeName,
                          std::span<const FieldDesc> fields,
                          void* obj);

template <class T>
bool renderComponent(const Reflection& r, T& obj) {
    return renderComponentByPtr(r, r.typeName<T>(), r.fieldsOf<T>(), &obj);
}

}  // namespace iron
