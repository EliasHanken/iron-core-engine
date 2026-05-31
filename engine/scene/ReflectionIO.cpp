#include "scene/ReflectionIO.h"

#include "core/Log.h"
#include "math/Quaternion.h"
#include "math/Vec.h"

#include <cstdint>
#include <string>

namespace iron {

namespace {

using json = nlohmann::json;

json vec3ToJson(const Vec3& v) { return json::array({v.x, v.y, v.z}); }
json quatToJson(const Quat& q) { return json::array({q.x, q.y, q.z, q.w}); }

void readVec3(const json& j, Vec3& out) {
    if (j.is_array() && j.size() == 3) {
        out.x = j[0].get<float>();
        out.y = j[1].get<float>();
        out.z = j[2].get<float>();
    }
}
void readQuat(const json& j, Quat& out) {
    if (j.is_array() && j.size() == 4) {
        out.x = j[0].get<float>();
        out.y = j[1].get<float>();
        out.z = j[2].get<float>();
        out.w = j[3].get<float>();
    }
}

// Look up an EnumValue by integer (round-tripping through int64_t) — used to
// serialize the current enum value as its registered name.
std::string_view enumValueName(std::span<const Reflection::EnumValue> values,
                               int64_t v) {
    for (const auto& ev : values)
        if (ev.value == v) return ev.name;
    return {};
}

}  // namespace

json componentToJsonByPtr(const Reflection& r,
                          std::span<const FieldDesc> fields,
                          const void* obj) {
    json out = json::object();
    for (const FieldDesc& f : fields) {
        switch (f.type) {
            case TypeId::Bool:
                out[std::string(f.name)] = *f.ptr<bool>(obj);
                break;
            case TypeId::Int32:
                out[std::string(f.name)] = *f.ptr<int32_t>(obj);
                break;
            case TypeId::UInt32:
                out[std::string(f.name)] = *f.ptr<uint32_t>(obj);
                break;
            case TypeId::UInt8:
                out[std::string(f.name)] = *f.ptr<uint8_t>(obj);
                break;
            case TypeId::Float:
                out[std::string(f.name)] = *f.ptr<float>(obj);
                break;
            case TypeId::String: {
                const std::string& s = *f.ptr<std::string>(obj);
                if (!s.empty()) out[std::string(f.name)] = s;  // omit empties
                break;
            }
            case TypeId::Vec3:
                out[std::string(f.name)] = vec3ToJson(*f.ptr<Vec3>(obj));
                break;
            case TypeId::Quat:
                out[std::string(f.name)] = quatToJson(*f.ptr<Quat>(obj));
                break;
            case TypeId::Enum: {
                auto vs = r.enumValuesById(f.enumTypeId);
                // Read enum value as its underlying int64 (assumes enum's
                // underlying type fits — true for all our enums).
                const int64_t v = static_cast<int64_t>(
                    *reinterpret_cast<const int32_t*>(
                        static_cast<const uint8_t*>(obj) + f.offset));
                const std::string_view name = enumValueName(vs, v);
                if (!name.empty())
                    out[std::string(f.name)] = std::string(name);
                else
                    Log::warn("ReflectionIO: enum '%.*s' value %lld has no registered name; omitting",
                              static_cast<int>(f.name.size()), f.name.data(),
                              static_cast<long long>(v));
                break;
            }
            case TypeId::OptionalEnum: {
                const auto& opt = *f.ptr<std::optional<int32_t>>(obj);
                if (opt.has_value()) {
                    auto vs = r.enumValuesById(f.enumTypeId);
                    const std::string_view name = enumValueName(
                        vs, static_cast<int64_t>(*opt));
                    if (!name.empty())
                        out[std::string(f.name)] = std::string(name);
                }
                break;
            }
            case TypeId::Unknown:
                Log::warn("ReflectionIO: field '%.*s' has Unknown TypeId; skipped",
                          static_cast<int>(f.name.size()), f.name.data());
                break;
        }
    }
    return out;
}

void componentFromJsonByPtr(const Reflection& r,
                            std::span<const FieldDesc> fields,
                            void* obj,
                            const json& j) {
    if (!j.is_object()) return;
    for (const FieldDesc& f : fields) {
        const std::string key(f.name);
        const bool present = j.contains(key);
        switch (f.type) {
            case TypeId::Bool:
                if (present && j[key].is_boolean()) *f.ptr<bool>(obj) = j[key].get<bool>();
                break;
            case TypeId::Int32:
                if (present && j[key].is_number_integer()) *f.ptr<int32_t>(obj) = j[key].get<int32_t>();
                break;
            case TypeId::UInt32:
                if (present && j[key].is_number_integer()) *f.ptr<uint32_t>(obj) = j[key].get<uint32_t>();
                break;
            case TypeId::UInt8:
                if (present && j[key].is_number_integer()) *f.ptr<uint8_t>(obj) = static_cast<uint8_t>(j[key].get<uint32_t>());
                break;
            case TypeId::Float:
                if (present && j[key].is_number()) *f.ptr<float>(obj) = j[key].get<float>();
                break;
            case TypeId::String:
                if (present && j[key].is_string()) *f.ptr<std::string>(obj) = j[key].get<std::string>();
                break;
            case TypeId::Vec3:
                if (present) readVec3(j[key], *f.ptr<Vec3>(obj));
                break;
            case TypeId::Quat:
                if (present) readQuat(j[key], *f.ptr<Quat>(obj));
                break;
            case TypeId::Enum: {
                if (!present || !j[key].is_string()) break;
                auto vs = r.enumValuesById(f.enumTypeId);
                const std::string s = j[key].get<std::string>();
                int64_t matched = vs.empty() ? 0 : vs[0].value;  // fallback: first
                bool found = false;
                for (const auto& ev : vs) {
                    if (ev.name == s) { matched = ev.value; found = true; break; }
                }
                if (!found && !vs.empty()) {
                    Log::warn("ReflectionIO: unknown enum value '%s' for field '%.*s'; defaulting to '%.*s'",
                              s.c_str(),
                              static_cast<int>(f.name.size()), f.name.data(),
                              static_cast<int>(vs[0].name.size()), vs[0].name.data());
                }
                *reinterpret_cast<int32_t*>(
                    static_cast<uint8_t*>(obj) + f.offset) = static_cast<int32_t>(matched);
                break;
            }
            case TypeId::OptionalEnum: {
                if (!present) {
                    f.ptr<std::optional<int32_t>>(obj)->reset();
                    break;
                }
                if (!j[key].is_string()) break;
                auto vs = r.enumValuesById(f.enumTypeId);
                const std::string s = j[key].get<std::string>();
                int64_t matched = vs.empty() ? 0 : vs[0].value;
                bool found = false;
                for (const auto& ev : vs) {
                    if (ev.name == s) { matched = ev.value; found = true; break; }
                }
                if (!found && !vs.empty()) {
                    Log::warn("ReflectionIO: unknown enum value '%s' for field '%.*s'; defaulting to '%.*s'",
                              s.c_str(),
                              static_cast<int>(f.name.size()), f.name.data(),
                              static_cast<int>(vs[0].name.size()), vs[0].name.data());
                }
                *f.ptr<std::optional<int32_t>>(obj) = static_cast<int32_t>(matched);
                break;
            }
            case TypeId::Unknown:
                break;
        }
    }
}

}  // namespace iron
