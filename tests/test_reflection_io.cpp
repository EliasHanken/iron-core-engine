#include "reflection/RegisterCoreTypes.h"
#include "reflection/Reflection.h"
#include "scene/ReflectionIO.h"
#include "scene/SceneFormat.h"
#include "world/Transform.h"

#include <nlohmann/json.hpp>

#include <cstdio>

using nlohmann::json;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static iron::Reflection makeReg() {
    iron::Reflection r;
    iron::registerTransform(r);
    iron::registerMeshRef(r);
    iron::registerMaterialDef(r);
    iron::registerRenderHandles(r);
    return r;
}

static void test_transform_roundtrip() {
    iron::Reflection r = makeReg();
    iron::Transform t;
    t.position = {1.0f, 2.0f, 3.0f};
    t.rotation = iron::Quat::fromAxisAngle(iron::Vec3{0, 1, 0}, 0.5f);
    t.scale    = {1.5f, 2.5f, 3.5f};

    json j = iron::componentToJson(r, t);
    iron::Transform back;
    iron::componentFromJson(r, back, j);

    CHECK(back.position.x == 1.0f);
    CHECK(back.position.y == 2.0f);
    CHECK(back.position.z == 3.0f);
    CHECK(back.scale.x    == 1.5f);
    CHECK(back.rotation.w == t.rotation.w);
}

static void test_mesh_ref_roundtrip_primitive_cube() {
    iron::Reflection r = makeReg();
    iron::MeshRef m;
    m.primitive = iron::PrimitiveKind::Cube;
    json j = iron::componentToJson(r, m);
    CHECK(j.contains("primitive"));
    CHECK(j["primitive"].get<std::string>() == "cube");

    iron::MeshRef back;
    iron::componentFromJson(r, back, j);
    CHECK(back.primitive.has_value());
    CHECK(back.primitive.value() == iron::PrimitiveKind::Cube);
}

static void test_mesh_ref_roundtrip_primitive_none_omits_key() {
    iron::Reflection r = makeReg();
    iron::MeshRef m;
    m.primitive = std::nullopt;
    m.gltfPath  = "assets/foo.gltf";
    json j = iron::componentToJson(r, m);
    CHECK(!j.contains("primitive"));     // nullopt -> omitted
    CHECK(j["gltfPath"].get<std::string>() == "assets/foo.gltf");

    iron::MeshRef back;
    iron::componentFromJson(r, back, j);
    CHECK(!back.primitive.has_value());
    CHECK(back.gltfPath == "assets/foo.gltf");
}

static void test_material_def_roundtrip_full() {
    iron::Reflection r = makeReg();
    iron::MaterialDef m;
    m.albedoPath   = "a.png";
    m.normalPath   = "n.png";
    m.specularPath = "s.png";
    m.emissive     = {0.4f, 0.5f, 0.6f};
    m.uvScale      = 2.0f;
    m.reflectivity = 0.7f;
    json j = iron::componentToJson(r, m);

    iron::MaterialDef back;
    iron::componentFromJson(r, back, j);
    CHECK(back.albedoPath   == "a.png");
    CHECK(back.normalPath   == "n.png");
    CHECK(back.specularPath == "s.png");
    CHECK(back.emissive.x   == 0.4f);
    CHECK(back.uvScale      == 2.0f);
    CHECK(back.reflectivity == 0.7f);
}

static void test_material_def_empty_strings_omitted() {
    iron::Reflection r = makeReg();
    iron::MaterialDef m;                        // all path fields empty
    m.emissive = {0.1f, 0.2f, 0.3f};
    json j = iron::componentToJson(r, m);
    CHECK(!j.contains("albedoPath"));
    CHECK(!j.contains("normalPath"));
    CHECK(!j.contains("specularPath"));
    CHECK(j["emissive"].is_array());
}

static void test_unknown_enum_name_logs_falls_back_to_first_value() {
    iron::Reflection r = makeReg();
    json j = {{"primitive", "bogus"}};
    iron::MeshRef back;
    iron::componentFromJson(r, back, j);
    // Spec: unknown enum string -> first registered value (cube).
    CHECK(back.primitive.has_value());
    CHECK(back.primitive.value() == iron::PrimitiveKind::Cube);
}

static void test_widget_hints_do_not_affect_serialization() {
    iron::Reflection r = makeReg();
    iron::MaterialDef m;
    m.emissive     = {0.3f, 0.3f, 0.3f};
    m.reflectivity = 0.5f;
    json j = iron::componentToJson(r, m);
    CHECK(j["emissive"].is_array());
    CHECK(j["emissive"].size() == 3u);
    CHECK(j["reflectivity"].is_number());
}

static void test_min_max_do_not_clamp_on_load() {
    iron::Reflection r = makeReg();
    json j = json::object();
    j["reflectivity"] = 1.5f;   // out of [0, 1]
    iron::MaterialDef back;
    iron::componentFromJson(r, back, j);
    CHECK(back.reflectivity == 1.5f);
}

int main() {
    test_transform_roundtrip();
    test_mesh_ref_roundtrip_primitive_cube();
    test_mesh_ref_roundtrip_primitive_none_omits_key();
    test_material_def_roundtrip_full();
    test_material_def_empty_strings_omitted();
    test_unknown_enum_name_logs_falls_back_to_first_value();
    test_widget_hints_do_not_affect_serialization();
    test_min_max_do_not_clamp_on_load();
    if (g_failures == 0) std::printf("All reflection-IO tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
