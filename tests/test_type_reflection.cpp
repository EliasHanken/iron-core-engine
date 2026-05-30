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

int main() {
    test_typeid_unknown_is_zero();
    test_fieldmeta_defaults_are_zero();
    test_fielddesc_ptr_arithmetic();
    test_typeidof_primitives();
    test_typeidof_string();
    test_typeidof_vec3_quat();
    test_typeidof_enum();
    test_typeidof_optional_enum();
    if (g_failures == 0) std::printf("All type-reflection tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
