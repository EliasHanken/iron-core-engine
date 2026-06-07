#include "nodes/NodeRegistry.h"

#include <algorithm>
#include <cctype>

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

namespace {
std::string toLower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return r;
}
bool isAllowed(const std::vector<std::string>* allow, const std::string& name) {
    if (!allow) return true;
    return std::find(allow->begin(), allow->end(), name) != allow->end();
}
}  // namespace

std::vector<NodeCreateGroup> buildCreateList(const NodeRegistry& registry,
                                             std::string_view query,
                                             const std::vector<std::string>* allowedTypeNames) {
    const std::string q = toLower(query);
    std::vector<const NodeTypeDesc*> types;
    for (const NodeTypeDesc* t : registry.all()) {
        if (!isAllowed(allowedTypeNames, t->typeName)) continue;
        if (!q.empty()) {
            const bool match = toLower(t->typeName).find(q) != std::string::npos ||
                               toLower(t->subtitle).find(q) != std::string::npos;
            if (!match) continue;
        }
        types.push_back(t);
    }
    std::sort(types.begin(), types.end(),
              [](const NodeTypeDesc* a, const NodeTypeDesc* b) { return a->typeName < b->typeName; });

    std::vector<NodeCreateGroup> groups;
    if (!q.empty()) { groups.push_back({"Results", std::move(types)}); return groups; }
    for (const NodeTypeDesc* t : types) {
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&](const NodeCreateGroup& g) { return g.category == t->category; });
        if (it == groups.end()) groups.push_back({t->category, {t}});
        else it->types.push_back(t);
    }
    std::sort(groups.begin(), groups.end(),
              [](const NodeCreateGroup& a, const NodeCreateGroup& b) { return a.category < b.category; });
    return groups;
}

}  // namespace iron
