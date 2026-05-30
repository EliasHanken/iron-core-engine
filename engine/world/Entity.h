#pragma once

#include <cstdint>

namespace iron {

// Generational entity handle. generation == 0 is the sentinel "no entity".
struct EntityId {
    uint32_t index = 0;
    uint32_t generation = 0;
    bool valid() const { return generation != 0; }
    bool operator==(const EntityId&) const = default;
};

inline constexpr EntityId kEntityNone{};

// Compile-time small-integer per type via a counter-template pattern.
// Returns the same value for the same T across translation units within
// one binary (function-local static initialisation), distinct across types.
namespace detail {
inline uint32_t nextComponentTypeId() {
    static uint32_t next = 0;
    return next++;
}
}  // namespace detail

template <class T>
inline uint32_t componentTypeId() {
    static const uint32_t id = detail::nextComponentTypeId();
    return id;
}

}  // namespace iron
