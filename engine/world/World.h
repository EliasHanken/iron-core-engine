#pragma once

#include "world/ComponentArray.h"
#include "world/Entity.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace iron {

class World {
public:
    static constexpr uint32_t kMaxComponentTypes = 256;

    EntityId create();
    void     destroy(EntityId e);
    bool     alive(EntityId e) const;

    // Deep copy. Snapshot/restore in editors (M41 Play/Stop) and tests rely
    // on this. Each ComponentArray is cloned via the virtual clone() above;
    // generations_ + freeList_ are POD-vector copies.
    World() = default;
    World(const World& other) { copyFrom(other); }
    World& operator=(const World& other) {
        if (this != &other) copyFrom(other);
        return *this;
    }
    World(World&&) = default;
    World& operator=(World&&) = default;

    template <class T> T*       add(EntityId e, const T& v = {}) {
        return arrayFor<T>().add(e, v);
    }
    template <class T> T*       get(EntityId e) {
        if (!alive(e)) return nullptr;
        auto* a = tryArrayFor<T>();
        return a ? a->get(e) : nullptr;
    }
    template <class T> const T* get(EntityId e) const {
        return const_cast<World*>(this)->get<T>(e);
    }
    template <class T> void remove(EntityId e) {
        if (auto* a = tryArrayFor<T>()) a->remove(e);
    }

    template <class T>
    ComponentArray<T>& view() {
        return arrayFor<T>();
    }

    template <class T>
    const ComponentArray<T>& view() const {
        return const_cast<World*>(this)->view<T>();
    }

private:
    struct IComponentArray {
        virtual ~IComponentArray() = default;
        virtual void remove(EntityId e) = 0;   // type-erased remove for destroy()
        virtual std::unique_ptr<IComponentArray> clone() const = 0;
    };

    template <class T>
    struct TypedComponentArray : IComponentArray, ComponentArray<T> {
        void remove(EntityId e) override { ComponentArray<T>::remove(e); }
        std::unique_ptr<IComponentArray> clone() const override {
            return std::make_unique<TypedComponentArray<T>>(*this);
        }
    };

    template <class T>
    TypedComponentArray<T>& arrayFor() {
        const uint32_t id = componentTypeId<T>();
        assert(id < kMaxComponentTypes && "Too many component types registered (raise kMaxComponentTypes)");
        if (!arrays_[id]) arrays_[id] = std::make_unique<TypedComponentArray<T>>();
        return static_cast<TypedComponentArray<T>&>(*arrays_[id]);
    }

    template <class T>
    TypedComponentArray<T>* tryArrayFor() {
        const uint32_t id = componentTypeId<T>();
        assert(id < kMaxComponentTypes && "Too many component types registered (raise kMaxComponentTypes)");
        return arrays_[id] ? static_cast<TypedComponentArray<T>*>(arrays_[id].get())
                           : nullptr;
    }

    void copyFrom(const World& other) {
        for (uint32_t i = 0; i < kMaxComponentTypes; ++i) {
            arrays_[i] = other.arrays_[i] ? other.arrays_[i]->clone() : nullptr;
        }
        generations_ = other.generations_;
        freeList_    = other.freeList_;
    }

    std::array<std::unique_ptr<IComponentArray>, kMaxComponentTypes> arrays_{};
    std::vector<uint32_t> generations_;
    std::vector<uint32_t> freeList_;
};

}  // namespace iron
