#include "nodes/NodeRegistry.h"

namespace iron {

namespace {
bool dataCompatible(PortType a, PortType b) {
    if (a == b) return true;
    const bool aNum = (a == PortType::Int || a == PortType::Float);
    const bool bNum = (b == PortType::Int || b == PortType::Float);
    return aNum && bNum;
}
}  // namespace

bool portsCompatible(const PortDesc& from, const PortDesc& to) {
    if (from.dir != PortDir::Out || to.dir != PortDir::In) return false;
    const bool fromExec = from.type == PortType::Exec;
    const bool toExec   = to.type   == PortType::Exec;
    if (fromExec != toExec) return false;
    if (!fromExec && !dataCompatible(from.type, to.type)) return false;
    return true;
}

void NodeRegistry::registerType(NodeTypeDesc desc) {
    types_[desc.typeName] = std::move(desc);
}

const NodeTypeDesc* NodeRegistry::find(std::string_view typeName) const {
    auto it = types_.find(std::string(typeName));
    return it == types_.end() ? nullptr : &it->second;
}

std::vector<const NodeTypeDesc*> NodeRegistry::all() const {
    std::vector<const NodeTypeDesc*> out;
    out.reserve(types_.size());
    for (const auto& [k, v] : types_) out.push_back(&v);
    return out;
}

nlohmann::json catalogToJson(const NodeRegistry& registry) {
    nlohmann::json arr = nlohmann::json::array();
    for (const NodeTypeDesc* t : registry.all()) {
        nlohmann::json ports = nlohmann::json::array();
        for (const PortDesc& p : t->ports) {
            ports.push_back({{"name", p.name},
                             {"type", portTypeName(p.type)},
                             {"dir", p.dir == PortDir::In ? "in" : "out"}});
        }
        arr.push_back({{"typeName", t->typeName},
                       {"category", t->category},
                       {"subtitle", t->subtitle},
                       {"devOnly",  t->devOnly},
                       {"ports", ports}});
    }
    return arr;
}

}  // namespace iron
