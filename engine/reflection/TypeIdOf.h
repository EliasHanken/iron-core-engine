#pragma once

#include "math/Quaternion.h"
#include "math/Vec.h"
#include "reflection/TypeId.h"

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

namespace iron {

// Member-pointer F -> TypeId mapping. v1 covers everything the four M37
// POD components need. Extend with new primary-template specializations
// when new field types appear; enum/optional are concept-constrained.

template <class F> struct TypeIdOf;

template <> struct TypeIdOf<bool>          { static constexpr TypeId v = TypeId::Bool; };
template <> struct TypeIdOf<int32_t>       { static constexpr TypeId v = TypeId::Int32; };
template <> struct TypeIdOf<uint32_t>      { static constexpr TypeId v = TypeId::UInt32; };
template <> struct TypeIdOf<uint8_t>       { static constexpr TypeId v = TypeId::UInt8; };
template <> struct TypeIdOf<float>         { static constexpr TypeId v = TypeId::Float; };
template <> struct TypeIdOf<std::string>   { static constexpr TypeId v = TypeId::String; };
template <> struct TypeIdOf<Vec3>          { static constexpr TypeId v = TypeId::Vec3; };
template <> struct TypeIdOf<Quat>          { static constexpr TypeId v = TypeId::Quat; };

template <class E> requires std::is_enum_v<E>
struct TypeIdOf<E> { static constexpr TypeId v = TypeId::Enum; };

template <class E> requires std::is_enum_v<E>
struct TypeIdOf<std::optional<E>> { static constexpr TypeId v = TypeId::OptionalEnum; };

}  // namespace iron
