#include "gameplay/ComponentNodes.h"

#include "gameplay/GameContext.h"
#include "nodes/NodeContext.h"
#include "nodes/NodeRegistry.h"
#include "reflection/FieldDesc.h"
#include "world/ComponentRegistry.h"
#include "world/ComponentSet.h"
#include "world/World.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iron {
namespace {

// TypeId -> PortType for the v1 supported set. nullopt = field gets no pin.
std::optional<PortType> portTypeFor(TypeId t) {
    switch (t) {
        case TypeId::Float:  return PortType::Float;
        case TypeId::Int32:  return PortType::Int;
        case TypeId::Bool:   return PortType::Bool;
        case TypeId::Vec3:   return PortType::Vec3;
        case TypeId::String: return PortType::String;
        default:             return std::nullopt;   // Quat/Enum/UInt*/... skipped
    }
}

// Resolve the running graph's self entity -> its runtime ComponentSet -> the
// component instance of `typeId`. nullptr on any miss (null domainContext in
// the editor preview, dead entity, no ComponentSet, component absent).
void* componentOf(NodeContext& c, std::uint32_t typeId) {
    auto* g = static_cast<GameContext*>(c.run().domainContext);
    if (!g || !g->world) return nullptr;
    ComponentSet* set = g->world->get<ComponentSet>(g->self);
    if (!set) return nullptr;
    for (const auto& box : set->all())
        if (box->typeId() == typeId) return box->data();
    return nullptr;
}

NodeValue readField(const void* obj, const FieldDesc& f) {
    switch (f.type) {
        case TypeId::Float:  return NodeValue::F(*f.ptr<float>(obj));
        case TypeId::Int32:  return NodeValue::I(*f.ptr<std::int32_t>(obj));
        case TypeId::Bool:   return NodeValue::B(*f.ptr<bool>(obj));
        case TypeId::Vec3:   return NodeValue::V3(*f.ptr<Vec3>(obj));
        case TypeId::String: return NodeValue::S(*f.ptr<std::string>(obj));
        default:             return NodeValue{};   // unreachable: pins are pre-filtered
    }
}

}  // namespace

void registerComponentNodes(NodeRegistry& nodes, const ComponentRegistry& components) {
    for (std::uint32_t typeId : components.order()) {
        const ComponentRegistry::Entry* e = components.byTypeId(typeId);
        if (!e) continue;
        const std::string compName(e->name);

        // Supported, non-hidden fields. Copies are safe: FieldDesc is a small
        // POD whose string_view name points at a static string literal.
        std::vector<FieldDesc> gettable;
        std::string subtitle;
        for (const FieldDesc& f : e->fields) {
            if (f.meta.hidden) continue;
            if (!portTypeFor(f.type)) continue;
            if (f.name == "has") continue;   // reserved for the Get node's has pin
            gettable.push_back(f);
            if (!subtitle.empty()) subtitle += ", ";
            subtitle += f.name;
        }

        // "Get <Component>": has + one output pin per field.
        NodeTypeDesc d;
        d.typeName = "Get " + compName;
        d.category = "Components";
        d.subtitle = subtitle;
        d.ports.push_back({"has", PortType::Bool, PortDir::Out});
        for (const FieldDesc& f : gettable)
            d.ports.push_back({std::string(f.name), *portTypeFor(f.type), PortDir::Out});
        d.evaluate = [typeId, gettable](NodeContext& c) {
            const void* obj = componentOf(c, typeId);
            c.out("has", NodeValue::B(obj != nullptr));
            for (const FieldDesc& f : gettable)
                c.out(f.name, obj ? readField(obj, f)
                                  : zeroValue(*portTypeFor(f.type)));
        };
        nodes.registerType(std::move(d));
    }
}

}  // namespace iron
