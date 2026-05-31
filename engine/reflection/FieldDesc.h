#pragma once

#include "reflection/TypeId.h"

#include <cstdint>
#include <string_view>

namespace iron {

// Per-field metadata. v1 covers range clamps + widget hints used by the
// Inspector dispatch and ignored by SceneIO.
struct FieldMeta {
    float min       = 0.0f;
    float max       = 0.0f;
    float dragSpeed = 0.0f;
    bool  color     = false;
    bool  slider    = false;
};

// One reflected field. Name has static storage duration (string literal
// from the sidecar .cpp); the registry stores it as a string_view.
struct FieldDesc {
    std::string_view name;
    TypeId           type   = TypeId::Unknown;
    uint32_t         offset = 0;
    FieldMeta        meta   = {};

    template <class T>
    T* ptr(void* obj) const {
        return reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + offset);
    }
    template <class T>
    const T* ptr(const void* obj) const {
        return reinterpret_cast<const T*>(
            static_cast<const uint8_t*>(obj) + offset);
    }
};

}  // namespace iron
