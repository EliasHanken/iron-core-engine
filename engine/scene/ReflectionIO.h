#pragma once

#include "reflection/Reflection.h"

#include <nlohmann/json.hpp>

// Generic component <-> JSON helpers driven by the iron::Reflection registry.
// Walks fieldsOf<T>() and dispatches per TypeId. Empty strings are omitted
// on write; nullopt enums are omitted entirely. Widget hints (color, slider,
// dragSpeed, min/max) DO NOT affect serialization.
//
// The Reflection& parameter is required so the dispatch can resolve enum
// value names via enumValuesById(FieldDesc::enumTypeId).

namespace iron {

template <class T>
nlohmann::json componentToJson(const Reflection& r, const T& obj);

template <class T>
void componentFromJson(const Reflection& r, T& obj, const nlohmann::json& j);

// Non-template byte-pointer workers — public so the templates above can be
// defined inline here without pulling the implementation into every TU.
nlohmann::json componentToJsonByPtr(const Reflection& r,
                                    std::span<const FieldDesc> fields,
                                    const void* obj);
void           componentFromJsonByPtr(const Reflection& r,
                                      std::span<const FieldDesc> fields,
                                      void* obj,
                                      const nlohmann::json& j);

template <class T>
nlohmann::json componentToJson(const Reflection& r, const T& obj) {
    return componentToJsonByPtr(r, r.fieldsOf<T>(), &obj);
}

template <class T>
void componentFromJson(const Reflection& r, T& obj, const nlohmann::json& j) {
    componentFromJsonByPtr(r, r.fieldsOf<T>(), &obj, j);
}

}  // namespace iron
