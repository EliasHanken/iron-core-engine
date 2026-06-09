#pragma once

#include "world/Entity.h"   // componentTypeId<T>()

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace iron {

// Type-erased, value-semantic holder for one component. clone() gives deep copy
// without needing a registry at copy time (mirrors World::IComponentArray).
struct IComponentBox {
    virtual ~IComponentBox() = default;
    virtual std::uint32_t typeId() const = 0;
    virtual std::unique_ptr<IComponentBox> clone() const = 0;
    virtual void*       data()       = 0;
    virtual const void* data() const = 0;
};

template <class T>
struct ComponentBox : IComponentBox {
    T value;
    ComponentBox() = default;
    explicit ComponentBox(const T& v) : value(v) {}
    std::uint32_t typeId() const override { return componentTypeId<T>(); }
    std::unique_ptr<IComponentBox> clone() const override {
        return std::make_unique<ComponentBox<T>>(value);
    }
    void*       data()       override { return &value; }
    const void* data() const override { return &value; }
};

// A per-entity bag of at most one component per type. Deep-copyable.
class ComponentSet {
public:
    ComponentSet() = default;
    ComponentSet(const ComponentSet& o) { copyFrom(o); }
    ComponentSet& operator=(const ComponentSet& o) {
        if (this != &o) copyFrom(o);
        return *this;
    }
    ComponentSet(ComponentSet&&) = default;
    ComponentSet& operator=(ComponentSet&&) = default;

    template <class T> T* add(const T& v = {}) {       // add-or-replace
        removeTypeId(componentTypeId<T>());
        comps_.push_back(std::make_unique<ComponentBox<T>>(v));
        return &static_cast<ComponentBox<T>*>(comps_.back().get())->value;
    }
    template <class T> T* get() {
        for (auto& b : comps_)
            if (b->typeId() == componentTypeId<T>())
                return static_cast<T*>(b->data());
        return nullptr;
    }
    template <class T> const T* get() const {
        return const_cast<ComponentSet*>(this)->get<T>();
    }
    template <class T> bool has() const { return hasTypeId(componentTypeId<T>()); }
    template <class T> void remove() { removeTypeId(componentTypeId<T>()); }

    bool hasTypeId(std::uint32_t id) const {
        for (auto& b : comps_) if (b->typeId() == id) return true;
        return false;
    }
    void removeTypeId(std::uint32_t id) {
        for (std::size_t i = 0; i < comps_.size(); ++i)
            if (comps_[i]->typeId() == id) { comps_.erase(comps_.begin() + i); return; }
    }
    void addBox(std::unique_ptr<IComponentBox> box) {
        if (!box) return;
        removeTypeId(box->typeId());
        comps_.push_back(std::move(box));
    }
    std::span<const std::unique_ptr<IComponentBox>> all() const { return comps_; }
    std::span<std::unique_ptr<IComponentBox>>       all()       { return comps_; }

private:
    void copyFrom(const ComponentSet& o) {
        comps_.clear();
        comps_.reserve(o.comps_.size());
        for (const auto& b : o.comps_) comps_.push_back(b->clone());
    }
    std::vector<std::unique_ptr<IComponentBox>> comps_;
};

}  // namespace iron
