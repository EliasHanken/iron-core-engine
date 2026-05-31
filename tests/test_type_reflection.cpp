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

static void test_reflection_field_by_name_hit() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("position", &iron::Transform::position)
        .field("rotation", &iron::Transform::rotation)
        .field("scale",    &iron::Transform::scale);
    const iron::FieldDesc* f = r.fieldByName<iron::Transform>("position");
    CHECK(f != nullptr);
    CHECK(f->name == "position");
    CHECK(f->type == iron::TypeId::Vec3);
}

static void test_reflection_field_by_name_miss() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("position", &iron::Transform::position);
    CHECK(r.fieldByName<iron::Transform>("nonexistent") == nullptr);
}

static void test_reflection_ptr_through_field_mutates_object() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("position", &iron::Transform::position);
    iron::Transform t{};
    const iron::FieldDesc* f = r.fieldByName<iron::Transform>("position");
    CHECK(f != nullptr);
    iron::Vec3* p = f->ptr<iron::Vec3>(&t);
    p->x = 1.0f;
    p->y = 2.0f;
    p->z = 3.0f;
    CHECK(t.position.x == 1.0f);
    CHECK(t.position.y == 2.0f);
    CHECK(t.position.z == 3.0f);
}

static void test_reflection_const_ptr_through_field() {
    iron::Reflection r;
    r.registerType<iron::Transform>("Transform")
        .field("position", &iron::Transform::position);
    iron::Transform t{};
    t.position = iron::Vec3{4.0f, 5.0f, 6.0f};
    const iron::FieldDesc* f = r.fieldByName<iron::Transform>("position");
    CHECK(f != nullptr);
    const iron::Vec3* p = f->ptr<iron::Vec3>(static_cast<const void*>(&t));
    CHECK(p->x == 4.0f);
    CHECK(p->y == 5.0f);
    CHECK(p->z == 6.0f);
}

#include "reflection/RegisterCoreTypes.h"
#include "render/RenderHandles.h"

static void test_register_transform_end_to_end() {
    iron::Reflection r;
    iron::registerTransform(r);
    CHECK(r.typeName<iron::Transform>() == "Transform");
    auto f = r.fieldsOf<iron::Transform>();
    CHECK(f.size() == 3);
    CHECK(f[0].name == "position");
    CHECK(f[1].name == "rotation");
    CHECK(f[2].name == "scale");
    CHECK(f[2].meta.min == 0.001f);   // scale lower bound
}

static void test_register_mesh_ref_end_to_end() {
    iron::Reflection r;
    iron::registerMeshRef(r);
    CHECK(r.typeName<iron::MeshRef>() == "MeshRef");
    auto f = r.fieldsOf<iron::MeshRef>();
    CHECK(f.size() == 2);
    CHECK(f[0].name == "primitive");
    CHECK(f[0].type == iron::TypeId::OptionalEnum);
    CHECK(f[1].name == "gltfPath");
    CHECK(f[1].type == iron::TypeId::String);
}

static void test_register_material_def_end_to_end() {
    iron::Reflection r;
    iron::registerMaterialDef(r);
    CHECK(r.typeName<iron::MaterialDef>() == "MaterialDef");
    auto f = r.fieldsOf<iron::MaterialDef>();
    CHECK(f.size() == 6);
    CHECK(f[0].name == "albedoPath");
    CHECK(f[3].name == "emissive");
    CHECK(f[3].type == iron::TypeId::Vec3);
    const iron::FieldDesc* refl = r.fieldByName<iron::MaterialDef>("reflectivity");
    CHECK(refl != nullptr);
    CHECK(refl->meta.min == 0.0f);
    CHECK(refl->meta.max == 1.0f);
}

static void test_register_render_handles_end_to_end() {
    iron::Reflection r;
    iron::registerRenderHandles(r);
    CHECK(r.typeName<iron::RenderHandles>() == "RenderHandles");
    auto f = r.fieldsOf<iron::RenderHandles>();
    CHECK(f.size() == 4);
    CHECK(f[0].name == "mesh");
    CHECK(f[0].type == iron::TypeId::UInt32);   // MeshHandle is uint32_t
    CHECK(f[1].name == "albedo");
    CHECK(f[1].type == iron::TypeId::UInt32);
}

static void test_register_all_four_in_one_registry() {
    iron::Reflection r;
    iron::registerTransform(r);
    iron::registerMeshRef(r);
    iron::registerMaterialDef(r);
    iron::registerRenderHandles(r);
    CHECK(!r.typeName<iron::Transform>().empty());
    CHECK(!r.typeName<iron::MeshRef>().empty());
    CHECK(!r.typeName<iron::MaterialDef>().empty());
    CHECK(!r.typeName<iron::RenderHandles>().empty());
}

static void test_fieldmeta_widget_hint_defaults() {
    iron::FieldMeta m;
    CHECK(m.dragSpeed == 0.0f);
    CHECK(m.color  == false);
    CHECK(m.slider == false);
}

static void test_field_records_widget_hints() {
    struct Probe { float a; iron::Vec3 b; };
    iron::Reflection r;
    r.registerType<Probe>("Probe")
        .field("a", &Probe::a, {.min = 0.0f, .max = 1.0f, .slider = true})
        .field("b", &Probe::b, {.color = true});
    auto fields = r.fieldsOf<Probe>();
    CHECK(fields.size() == 2);
    CHECK(fields[0].meta.slider == true);
    CHECK(fields[0].meta.min == 0.0f);
    CHECK(fields[0].meta.max == 1.0f);
    CHECK(fields[1].meta.color == true);
    CHECK(fields[1].meta.dragSpeed == 0.0f);
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
    test_reflection_field_by_name_hit();
    test_reflection_field_by_name_miss();
    test_reflection_ptr_through_field_mutates_object();
    test_reflection_const_ptr_through_field();
    test_register_transform_end_to_end();
    test_register_mesh_ref_end_to_end();
    test_register_material_def_end_to_end();
    test_register_render_handles_end_to_end();
    test_register_all_four_in_one_registry();
    test_fieldmeta_widget_hint_defaults();
    test_field_records_widget_hints();
    if (g_failures == 0) std::printf("All type-reflection tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
