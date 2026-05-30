#pragma once

#include "world/ComponentArray.h"
#include "world/Entity.h"

#include <array>
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
        const auto* a = const_cast<World*>(this)->tryArrayFor<T>();
        if (a) return *a;
        // Lazy-create empty array so const view() always returns a valid ref.
        const_cast<World*>(this)->arrayFor<T>();
        return *const_cast<World*>(this)->tryArrayFor<T>();
    }

private:
    struct IComponentArray {
        virtual ~IComponentArray() = default;
        virtual void remove(EntityId e) = 0;   // type-erased remove for destroy()
    };

    template <class T>
    struct TypedComponentArray : IComponentArray, ComponentArray<T> {
        void remove(EntityId e) override { ComponentArray<T>::remove(e); }
    };

    template <class T>
    TypedComponentArray<T>& arrayFor() {
        const uint32_t id = componentTypeId<T>();
        if (!arrays_[id]) arrays_[id] = std::make_unique<TypedComponentArray<T>>();
        return static_cast<TypedComponentArray<T>&>(*arrays_[id]);
    }

    template <class T>
    TypedComponentArray<T>* tryArrayFor() {
        const uint32_t id = componentTypeId<T>();
        return arrays_[id] ? static_cast<TypedComponentArray<T>*>(arrays_[id].get())
                           : nullptr;
    }

    std::array<std::unique_ptr<IComponentArray>, kMaxComponentTypes> arrays_{};
    std::vector<uint32_t> generations_;
    std::vector<uint32_t> freeList_;
};

}  // namespace iron
