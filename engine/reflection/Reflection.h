#pragma once

#include "reflection/FieldDesc.h"
#include "reflection/TypeIdOf.h"
#include "world/Entity.h"   // componentTypeId<T>()

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace iron {

// Host-owned type registry. Reuses M37's componentTypeId<T>() for the
// per-type integer key (256-type cap is generous at this scale).
class Reflection {
public:
    static constexpr uint32_t kMaxTypes = 256;

    template <class T>
    class TypeBuilder {
    public:
        template <class F>
        TypeBuilder& field(std::string_view name, F T::* member, FieldMeta meta = {}) {
            // Offset computation: a transient default-constructed T plus
            // pointer arithmetic. Well-defined for any addressable non-static
            // data member of a default-constructible T; works in practice on
            // MSVC, GCC, and Clang for both standard-layout and non-SL types
            // (e.g. std::string / std::optional members).
            T probe{};
            const uint32_t off = static_cast<uint32_t>(
                reinterpret_cast<const uint8_t*>(&(probe.*member)) -
                reinterpret_cast<const uint8_t*>(&probe));
            reg_.types_[typeId_].fields.push_back(
                FieldDesc{ name, TypeIdOf<F>::v, off, meta, enumTypeIdOf<F>() });
            return *this;
        }

    private:
        friend class Reflection;
        TypeBuilder(Reflection& r, uint32_t typeId) : reg_(r), typeId_(typeId) {}
        Reflection& reg_;
        uint32_t    typeId_;
    };

    template <class T>
    TypeBuilder<T> registerType(std::string_view name) {
        // Note: standard-layout is NOT required — the offset probe uses
        // pointer arithmetic on a live default-constructed T, which is
        // well-defined for any addressable non-static data member.
        // (MSVC's std::string / std::optional are not standard-layout, yet
        // are reflectable here.)
        static_assert(std::is_default_constructible_v<T>,
            "Reflection requires default-constructible types (the offset probe is T{}).");
        const uint32_t id = componentTypeId<T>();
        assert(id < kMaxTypes && "Too many reflected types (raise Reflection::kMaxTypes)");
        types_[id].name = name;
        types_[id].fields.clear();
        registered_[id] = true;
        return TypeBuilder<T>(*this, id);
    }

    template <class T>
    std::span<const FieldDesc> fieldsOf() const {
        const uint32_t id = componentTypeId<T>();
        return registered_[id] ? std::span<const FieldDesc>(types_[id].fields)
                               : std::span<const FieldDesc>{};
    }

    template <class T>
    std::string_view typeName() const {
        const uint32_t id = componentTypeId<T>();
        return registered_[id] ? types_[id].name : std::string_view{};
    }

    template <class T>
    const FieldDesc* fieldByName(std::string_view name) const {
        for (const FieldDesc& f : fieldsOf<T>())
            if (f.name == name) return &f;
        return nullptr;
    }

    // ── Enum registry ──────────────────────────────────────────────────────
    struct EnumValue {
        std::string_view name;
        int64_t          value;
    };

    template <class E>
    class EnumBuilder {
    public:
        EnumBuilder& value(std::string_view name, E v) {
            static_assert(std::is_enum_v<E>, "EnumBuilder requires an enum type");
            // v1 limit: ReflectionIO's enum dispatch reads/writes the field
            // payload as int32_t. Future enums with a different underlying
            // type need a templated dispatch — fail loudly here until that
            // exists.
            static_assert(sizeof(std::underlying_type_t<E>) == sizeof(int32_t),
                "ReflectionIO v1: enum underlying type must be int32_t (see ReflectionIO.cpp)");
            reg_.enums_[typeId_].values.push_back(
                EnumValue{ name, static_cast<int64_t>(
                    static_cast<std::underlying_type_t<E>>(v)) });
            return *this;
        }
    private:
        friend class Reflection;
        EnumBuilder(Reflection& r, uint32_t id) : reg_(r), typeId_(id) {}
        Reflection& reg_;
        uint32_t    typeId_;
    };

    template <class E>
    EnumBuilder<E> registerEnum(std::string_view name) {
        static_assert(std::is_enum_v<E>, "registerEnum requires an enum type");
        const uint32_t id = componentTypeId<E>();
        assert(id < kMaxTypes && "Too many reflected enums (raise Reflection::kMaxTypes)");
        enums_[id].name = name;
        enums_[id].values.clear();
        enumsRegistered_[id] = true;
        return EnumBuilder<E>(*this, id);
    }

    template <class E>
    std::span<const EnumValue> enumValues() const {
        static_assert(std::is_enum_v<E>, "enumValues requires an enum type");
        return enumValuesById(componentTypeId<E>());
    }

    template <class E>
    std::string_view enumName() const {
        static_assert(std::is_enum_v<E>, "enumName requires an enum type");
        return enumNameById(componentTypeId<E>());
    }

    // Dispatch-side, non-template lookups (Inspector / SceneIO call these via
    // FieldDesc::enumTypeId without knowing the concrete enum type E).
    std::span<const EnumValue> enumValuesById(uint32_t id) const {
        return (id < kMaxTypes && enumsRegistered_[id])
            ? std::span<const EnumValue>(enums_[id].values)
            : std::span<const EnumValue>{};
    }
    std::string_view enumNameById(uint32_t id) const {
        return (id < kMaxTypes && enumsRegistered_[id])
            ? enums_[id].name
            : std::string_view{};
    }

private:
    struct TypeEntry {
        std::string_view       name;
        std::vector<FieldDesc> fields;
    };
    std::array<TypeEntry, kMaxTypes> types_{};
    std::array<bool, kMaxTypes>      registered_{};

    // Both name and values[*].name must outlive this registry entry
    // (string literals from sidecar .cpp satisfy this — same contract as
    // FieldDesc::name and TypeEntry::name).
    struct EnumEntry {
        std::string_view       name;
        std::vector<EnumValue> values;
    };
    std::array<EnumEntry, kMaxTypes> enums_{};
    std::array<bool, kMaxTypes>      enumsRegistered_{};
};

}  // namespace iron
