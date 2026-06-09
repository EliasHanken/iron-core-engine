#include "editor/ReflectionInspector.h"

#include "core/Log.h"
#include "math/Quaternion.h"
#include "math/Vec.h"

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iron {

namespace {

constexpr float kVec3DefaultSpeed  = 0.05f;
constexpr float kFloatDefaultSpeed = 0.05f;
constexpr float kEulerDefaultSpeed = 0.5f;

bool drawString(const FieldDesc& f, void* obj) {
    // Resizable backing so long paths don't get silently truncated. ImGui::InputText
    // writes up to size()-1 chars + null; we headroom-pad to (current + 128, min 256)
    // so users can keep typing without hitting the cap on a typical edit.
    auto* s = f.ptr<std::string>(obj);
    std::string buf = *s;
    buf.resize(std::max(buf.size() + 128, size_t{256}));
    if (ImGui::InputText(std::string(f.name).c_str(), buf.data(), buf.size())) {
        *s = buf.c_str();   // trim at first null
        return true;
    }
    return false;
}

bool drawFloat(const FieldDesc& f, void* obj) {
    auto* v = f.ptr<float>(obj);
    const std::string label(f.name);
    if (f.meta.slider && (f.meta.min != f.meta.max))
        return ImGui::SliderFloat(label.c_str(), v, f.meta.min, f.meta.max);
    const float speed = f.meta.dragSpeed > 0.0f ? f.meta.dragSpeed : kFloatDefaultSpeed;
    return ImGui::DragFloat(label.c_str(), v, speed, f.meta.min, f.meta.max);
}

bool drawVec3(const FieldDesc& f, void* obj) {
    auto* v = f.ptr<Vec3>(obj);
    const std::string label(f.name);
    if (f.meta.color)
        return ImGui::ColorEdit3(label.c_str(), &v->x);
    const float speed = f.meta.dragSpeed > 0.0f ? f.meta.dragSpeed : kVec3DefaultSpeed;
    return ImGui::DragFloat3(label.c_str(), &v->x, speed, f.meta.min, f.meta.max);
}

bool drawQuat(const FieldDesc& f, void* obj) {
    auto* q = f.ptr<Quat>(obj);
    Vec3 euler = quatToEuler(*q);
    const float speed = f.meta.dragSpeed > 0.0f ? f.meta.dragSpeed : kEulerDefaultSpeed;
    if (ImGui::DragFloat3(std::string(f.name).c_str(), &euler.x, speed)) {
        *q = eulerToQuat(euler);
        return true;
    }
    return false;
}

bool drawBool(const FieldDesc& f, void* obj) {
    return ImGui::Checkbox(std::string(f.name).c_str(), f.ptr<bool>(obj));
}

bool drawIntLike(const FieldDesc& f, void* obj) {
    const std::string label(f.name);
    int32_t v = 0;
    switch (f.type) {
        case TypeId::Int32:  v = *f.ptr<int32_t>(obj); break;
        case TypeId::UInt32: v = static_cast<int32_t>(*f.ptr<uint32_t>(obj)); break;
        case TypeId::UInt8:  v = static_cast<int32_t>(*f.ptr<uint8_t>(obj));  break;
        default: break;
    }
    if (!ImGui::InputInt(label.c_str(), &v)) return false;
    if (f.type == TypeId::UInt8) {
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
    } else if (f.meta.min != f.meta.max) {
        if (v < static_cast<int32_t>(f.meta.min)) v = static_cast<int32_t>(f.meta.min);
        if (v > static_cast<int32_t>(f.meta.max)) v = static_cast<int32_t>(f.meta.max);
    }
    switch (f.type) {
        case TypeId::Int32:  *f.ptr<int32_t>(obj)  = v; break;
        case TypeId::UInt32: *f.ptr<uint32_t>(obj) = static_cast<uint32_t>(v); break;
        case TypeId::UInt8:  *f.ptr<uint8_t>(obj)  = static_cast<uint8_t>(v);  break;
        default: break;
    }
    return true;
}

// Build a flat null-terminated buffer of "name1\0name2\0..." for ImGui::Combo.
struct ComboNames {
    std::vector<char> chars;
};

ComboNames buildComboNames(std::span<const Reflection::EnumValue> values,
                           bool prependNone) {
    ComboNames out;
    if (prependNone) {
        out.chars.insert(out.chars.end(), {'N', 'o', 'n', 'e', '\0'});
    }
    for (const auto& v : values) {
        out.chars.insert(out.chars.end(), v.name.begin(), v.name.end());
        out.chars.push_back('\0');
    }
    out.chars.push_back('\0');
    return out;
}

bool drawEnum(const Reflection& r, const FieldDesc& f, void* obj) {
    auto values = r.enumValuesById(f.enumTypeId);
    if (values.empty()) {
        ImGui::TextDisabled("%s (no enum registered)", std::string(f.name).c_str());
        return false;
    }
    const int32_t cur = *reinterpret_cast<int32_t*>(
        static_cast<uint8_t*>(obj) + f.offset);
    int picked = -1;
    for (size_t i = 0; i < values.size(); ++i) {
        if (static_cast<int32_t>(values[i].value) == cur) { picked = static_cast<int>(i); break; }
    }
    if (picked < 0) {
        Log::warn("ReflectionInspector: enum field '%.*s' has unregistered value %d; showing index 0",
                  static_cast<int>(f.name.size()), f.name.data(),
                  static_cast<int>(cur));
        picked = 0;
    }
    ComboNames names = buildComboNames(values, /*prependNone=*/false);
    if (!ImGui::Combo(std::string(f.name).c_str(), &picked, names.chars.data())) return false;
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(obj) + f.offset) =
        static_cast<int32_t>(values[picked].value);
    return true;
}

bool drawOptionalEnum(const Reflection& r, const FieldDesc& f, void* obj) {
    auto values = r.enumValuesById(f.enumTypeId);
    auto* opt = f.ptr<std::optional<int32_t>>(obj);
    int picked = 0;   // index 0 = "None"
    if (opt->has_value()) {
        for (size_t i = 0; i < values.size(); ++i) {
            if (static_cast<int32_t>(values[i].value) == *opt) {
                picked = static_cast<int>(i) + 1;
                break;
            }
        }
    }
    ComboNames names = buildComboNames(values, /*prependNone=*/true);
    if (!ImGui::Combo(std::string(f.name).c_str(), &picked, names.chars.data())) return false;
    if (picked == 0) {
        opt->reset();
    } else {
        *opt = static_cast<int32_t>(values[picked - 1].value);
    }
    return true;
}

}  // namespace

bool renderComponentByPtr(const Reflection& r,
                          std::string_view typeName,
                          std::span<const FieldDesc> fields,
                          void* obj) {
    if (!typeName.empty()) ImGui::SeparatorText(std::string(typeName).c_str());
    bool changed = false;
    for (const FieldDesc& f : fields) {
        if (f.meta.hidden) continue;
        switch (f.type) {
            case TypeId::Bool:   changed |= drawBool(f, obj);          break;
            case TypeId::Int32:
            case TypeId::UInt32:
            case TypeId::UInt8:  changed |= drawIntLike(f, obj);       break;
            case TypeId::Float:  changed |= drawFloat(f, obj);         break;
            case TypeId::String: changed |= drawString(f, obj);        break;
            case TypeId::Vec3:   changed |= drawVec3(f, obj);          break;
            case TypeId::Quat:   changed |= drawQuat(f, obj);          break;
            case TypeId::Enum:   changed |= drawEnum(r, f, obj);       break;
            case TypeId::OptionalEnum:
                                 changed |= drawOptionalEnum(r, f, obj); break;
            case TypeId::Unknown:
                ImGui::TextDisabled("%s (unknown type)", std::string(f.name).c_str());
                break;
        }
    }
    return changed;
}

}  // namespace iron
