#pragma once

#include <cstdint>

namespace iron {

// Reflection type tags. v1 covers what the four M37 POD components need;
// extend the enum as new field types appear.
enum class TypeId : uint8_t {
    Unknown,
    Bool,
    Int32,
    UInt32,
    UInt8,
    Float,
    String,
    Vec3,
    Quat,
    Enum,
    OptionalEnum,
};

}  // namespace iron
