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
            // pointer arithmetic. Well-defined for standard-layout types
            // (the static_assert in registerType<T> guards this).
            T probe{};
            const uint32_t off = static_cast<uint32_t>(
                reinterpret_cast<const uint8_t*>(&(probe.*member)) -
                reinterpret_cast<const uint8_t*>(&probe));
            reg_.types_[typeId_].fields.push_back(
                FieldDesc{ name, TypeIdOf<F>::v, off, meta });
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

private:
    struct TypeEntry {
        std::string_view       name;
        std::vector<FieldDesc> fields;
    };
    std::array<TypeEntry, kMaxTypes> types_{};
    std::array<bool, kMaxTypes>      registered_{};
};

}  // namespace iron
