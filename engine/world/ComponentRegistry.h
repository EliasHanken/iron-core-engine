#pragma once

#include "reflection/FieldDesc.h"
#include "reflection/Reflection.h"
#include "world/ComponentSet.h"
#include "world/Entity.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iron {

// Registry of authorable component TYPES, layered on Reflection. Register a type
// once (it must already be Reflection::registerType<T>()'d) and it becomes
// available to the Inspector's Add-Component menu and to SceneIO generically.
class ComponentRegistry {
public:
    struct Entry {
        std::uint32_t              typeId = 0;
        std::string_view           name;
        std::span<const FieldDesc> fields;   // lives in the Reflection instance
        std::function<std::unique_ptr<IComponentBox>()> factory;
    };

    template <class T>
    void registerComponent(std::string_view name, const Reflection& r) {
        Entry e;
        e.typeId  = componentTypeId<T>();
        e.name    = name;
        e.fields  = r.fieldsOf<T>();
        e.factory = [] { return std::unique_ptr<IComponentBox>(
                             std::make_unique<ComponentBox<T>>()); };
        if (entries_.find(e.typeId) == entries_.end()) order_.push_back(e.typeId);
        entries_[e.typeId] = std::move(e);
    }

    const std::vector<std::uint32_t>& order() const { return order_; }

    const Entry* byTypeId(std::uint32_t id) const {
        auto it = entries_.find(id);
        return it == entries_.end() ? nullptr : &it->second;
    }
    const Entry* byName(std::string_view name) const {
        for (const auto& [id, e] : entries_) if (e.name == name) return &e;
        return nullptr;
    }

private:
    std::unordered_map<std::uint32_t, Entry> entries_;
    std::vector<std::uint32_t> order_;
};

}  // namespace iron
