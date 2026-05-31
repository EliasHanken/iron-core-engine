#pragma once

#include "math/Quaternion.h"
#include "math/Vec.h"
#include "reflection/TypeId.h"
#include "world/Entity.h"   // componentTypeId<T>()

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

// Detect std::optional<E> where E is an enum (e.g. MeshRef::primitive).
template <class F>
inline constexpr bool is_optional_enum_v = false;

template <class E>
inline constexpr bool is_optional_enum_v<std::optional<E>> = std::is_enum_v<E>;

// Return the enum's registry id for enum-bearing field types; 0 otherwise.
// Used by the Inspector / SceneIO dispatch to look up value names without
// knowing the concrete enum type E.
template <class F>
constexpr uint32_t enumTypeIdOf() {
    if constexpr (std::is_enum_v<F>)
        return componentTypeId<F>();
    else if constexpr (is_optional_enum_v<F>)
        return componentTypeId<typename F::value_type>();
    else
        return 0u;
}

}  // namespace iron
