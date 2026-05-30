#include "reflection/TypeId.h"
#include "reflection/FieldDesc.h"

#include <cstdint>
#include <cstdio>

#include "reflection/TypeIdOf.h"
#include "math/Vec.h"
#include "math/Quaternion.h"
#include "scene/SceneFormat.h"   // for PrimitiveKind enum in deduction tests

#include <optional>
#include <string>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void test_typeid_unknown_is_zero() {
    CHECK(static_cast<int>(iron::TypeId::Unknown) == 0);
}

static void test_fieldmeta_defaults_are_zero() {
    iron::FieldMeta m;
    CHECK(m.min == 0.0f);
    CHECK(m.max == 0.0f);
}

static void test_fielddesc_ptr_arithmetic() {
    // Simulate a struct with two fields and verify ptr<T> computes the right
    // address via the stored byte offset.
    struct Probe { int a; float b; };
    Probe p{};
    iron::FieldDesc fa{ "a", iron::TypeId::Int32,
                        static_cast<uint32_t>(offsetof(Probe, a)), {} };
    iron::FieldDesc fb{ "b", iron::TypeId::Float,
                        static_cast<uint32_t>(offsetof(Probe, b)), {} };
    CHECK(fa.ptr<int>(&p) == &p.a);
    CHECK(fb.ptr<float>(&p) == &p.b);
    *fa.ptr<int>(&p)   = 42;
    *fb.ptr<float>(&p) = 1.5f;
    CHECK(p.a == 42);
    CHECK(p.b == 1.5f);
}

static void test_typeidof_primitives() {
    CHECK(iron::TypeIdOf<bool>::v     == iron::TypeId::Bool);
    CHECK(iron::TypeIdOf<int32_t>::v  == iron::TypeId::Int32);
    CHECK(iron::TypeIdOf<uint32_t>::v == iron::TypeId::UInt32);
    CHECK(iron::TypeIdOf<uint8_t>::v  == iron::TypeId::UInt8);
    CHECK(iron::TypeIdOf<float>::v    == iron::TypeId::Float);
}

static void test_typeidof_string() {
    CHECK(iron::TypeIdOf<std::string>::v == iron::TypeId::String);
}

static void test_typeidof_vec3_quat() {
    CHECK(iron::TypeIdOf<iron::Vec3>::v == iron::TypeId::Vec3);
    CHECK(iron::TypeIdOf<iron::Quat>::v == iron::TypeId::Quat);
}

static void test_typeidof_enum() {
    CHECK(iron::TypeIdOf<iron::PrimitiveKind>::v == iron::TypeId::Enum);
}

static void test_typeidof_optional_enum() {
    CHECK(iron::TypeIdOf<std::optional<iron::PrimitiveKind>>::v
          == iron::TypeId::OptionalEnum);
}

#include "reflection/Reflection.h"
#include "world/Transform.h"

static void test_reflection_register_type_stores_name() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform");
    CHECK(r.typeName<iron::Transform>() == "Transform");
}

static void test_reflection_unregistered_type_has_empty_name() {
    iron::Reflection r;
    CHECK(r.typeName<iron::Transform>().empty());
}

static void test_reflection_field_offsets_match_offsetof() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("position", &iron::Transform::position)
        .field("rotation", &iron::Transform::rotation)
        .field("scale",    &iron::Transform::scale);
    auto fields = r.fieldsOf<iron::Transform>();
    CHECK(fields.size() == 3);
    CHECK(fields[0].name == "position");
    CHECK(fields[1].name == "rotation");
    CHECK(fields[2].name == "scale");
    CHECK(fields[0].offset == offsetof(iron::Transform, position));
    CHECK(fields[1].offset == offsetof(iron::Transform, rotation));
    CHECK(fields[2].offset == offsetof(iron::Transform, scale));
    CHECK(fields[0].type == iron::TypeId::Vec3);
    CHECK(fields[1].type == iron::TypeId::Quat);
    CHECK(fields[2].type == iron::TypeId::Vec3);
}

static void test_reflection_field_meta_roundtrip() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("scale", &iron::Transform::scale, {.min = 0.001f, .max = 1000.0f});
    auto fields = r.fieldsOf<iron::Transform>();
    CHECK(fields.size() == 1);
    CHECK(fields[0].meta.min == 0.001f);
    CHECK(fields[0].meta.max == 1000.0f);
}

int main() {
    test_typeid_unknown_is_zero();
    test_fieldmeta_defaults_are_zero();
    test_fielddesc_ptr_arithmetic();
    test_typeidof_primitives();
    test_typeidof_string();
    test_typeidof_vec3_quat();
    test_typeidof_enum();
    test_typeidof_optional_enum();
    test_reflection_register_type_stores_name();
    test_reflection_unregistered_type_has_empty_name();
    test_reflection_field_offsets_match_offsetof();
    test_reflection_field_meta_roundtrip();
    if (g_failures == 0) std::printf("All type-reflection tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
