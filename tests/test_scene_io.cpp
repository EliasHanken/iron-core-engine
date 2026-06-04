#include "scene/SceneFormat.h"
#include "scene/SceneIO.h"
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "reflection/Reflection.h"
#include "reflection/RegisterCoreTypes.h"
#include "world/CollisionShape.h"
#include "audio/AudioEmitter.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace iron;
namespace fs = std::filesystem;

namespace {

std::string tempScenePath(const char* name) {
    return (fs::temp_directory_path() / name).string();
}

iron::Reflection makeReflectionRegistry() {
    iron::Reflection r;
    iron::registerTransform(r);
    iron::registerMeshRef(r);
    iron::registerMaterialDef(r);
    iron::registerRenderHandles(r);
    iron::registerCollisionShape(r);
    iron::registerAudioEmitter(r);
    iron::registerReflectionProbe(r);
    return r;
}

SceneFile makeSampleScene() {
    SceneFile s;
    s.clearColor = {0.2f, 0.3f, 0.4f};
    s.sun.direction = {-0.4f, -1.0f, -0.3f};
    s.sun.color     = {1.0f, 0.95f, 0.9f};
    s.sun.ambient   = 0.15f;
    s.fog.color   = {0.5f, 0.5f, 0.6f};
    s.fog.density = 0.02f;

    PointLight pl;
    pl.position  = {0.0f, 3.0f, 0.0f};
    pl.color     = {1.0f, 0.5f, 0.2f};
    pl.intensity = 2.0f;
    pl.range     = 12.0f;
    s.pointLights.push_back(pl);

    SceneEntity floor;
    floor.name     = "floor";
    floor.transform.position = {0.0f, 0.0f, 0.0f};
    floor.transform.scale    = {20.0f, 1.0f, 20.0f};
    floor.mesh.primitive = PrimitiveKind::Plane;
    floor.material.uvScale = 8.0f;
    s.entities.push_back(floor);

    SceneEntity helmet;
    helmet.name     = "helmet";
    helmet.transform.position = {2.0f, 1.5f, 0.0f};
    helmet.transform.rotation = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.7f);
    helmet.transform.scale    = {1.5f, 1.5f, 1.5f};
    helmet.mesh.gltfPath        = "assets/damaged-helmet/DamagedHelmet.gltf";
    helmet.material.emissive    = {0.1f, 0.0f, 0.0f};
    helmet.material.reflectivity = 0.3f;
    s.entities.push_back(helmet);
    return s;
}

}  // namespace

int main() {
    // --- Test 1: round-trip preserves every field ---
    {
        const SceneFile original = makeSampleScene();
        const std::string path = tempScenePath("iron_scene_roundtrip.json");
        const iron::Reflection r = makeReflectionRegistry();
        CHECK(saveSceneFile(r, original, path));

        const auto loadedOpt = loadSceneFile(r, path);
        CHECK(loadedOpt.has_value());
        if (loadedOpt.has_value()) {
            const SceneFile& l = *loadedOpt;
            CHECK_NEAR(l.clearColor.x, 0.2f);
            CHECK_NEAR(l.clearColor.z, 0.4f);
            CHECK_NEAR(l.sun.direction.y, -1.0f);
            CHECK_NEAR(l.sun.ambient, 0.15f);
            CHECK_NEAR(l.fog.density, 0.02f);
            CHECK(l.pointLights.size() == 1u);
            if (!l.pointLights.empty()) {
                CHECK_NEAR(l.pointLights[0].range, 12.0f);
                CHECK_NEAR(l.pointLights[0].intensity, 2.0f);
            }
            CHECK(l.entities.size() == 2u);
            if (l.entities.size() == 2u) {
                CHECK(l.entities[0].name == "floor");
                CHECK(l.entities[0].mesh.primitive.has_value());
                CHECK(l.entities[0].mesh.primitive.value() == PrimitiveKind::Plane);
                CHECK(l.entities[0].mesh.gltfPath.empty());
                CHECK_NEAR(l.entities[0].transform.scale.x, 20.0f);
                CHECK_NEAR(l.entities[0].material.uvScale, 8.0f);
                CHECK(l.entities[1].name == "helmet");
                CHECK(!l.entities[1].mesh.primitive.has_value());
                CHECK(l.entities[1].mesh.gltfPath == "assets/damaged-helmet/DamagedHelmet.gltf");
                CHECK_NEAR(l.entities[1].material.reflectivity, 0.3f);
                const Quat q = l.entities[1].transform.rotation;
                const Quat e = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.7f);
                CHECK_NEAR(q.x, e.x);
                CHECK_NEAR(q.y, e.y);
                CHECK_NEAR(q.z, e.z);
                CHECK_NEAR(q.w, e.w);
            }
        }
        fs::remove(path);
    }

    // --- Test 2: malformed JSON returns nullopt ---
    {
        const std::string path = tempScenePath("iron_scene_malformed.json");
        { std::ofstream f(path); f << "{ this is not valid json ]"; }
        const iron::Reflection r = makeReflectionRegistry();
        const auto loaded = loadSceneFile(r, path);
        CHECK(!loaded.has_value());
        fs::remove(path);
    }

    // --- Test 3: missing file returns nullopt ---
    {
        const iron::Reflection r = makeReflectionRegistry();
        const auto loaded = loadSceneFile(r, "does/not/exist/scene.json");
        CHECK(!loaded.has_value());
    }

    // --- Test 4: minimal scene uses defaults for omitted fields ---
    {
        const std::string path = tempScenePath("iron_scene_minimal.json");
        {
            std::ofstream f(path);
            f << R"({ "entities": [ { "name": "c", "mesh": { "primitive": "cube" } } ] })";
        }
        const iron::Reflection r = makeReflectionRegistry();
        const auto loadedOpt = loadSceneFile(r, path);
        CHECK(loadedOpt.has_value());
        if (loadedOpt.has_value()) {
            const SceneFile& l = *loadedOpt;
            CHECK(l.entities.size() == 1u);
            if (!l.entities.empty()) {
                CHECK(l.entities[0].mesh.primitive.has_value());
                CHECK(l.entities[0].mesh.primitive.value() == PrimitiveKind::Cube);
                CHECK_NEAR(l.entities[0].transform.position.x, 0.0f);
                CHECK_NEAR(l.entities[0].transform.scale.x, 1.0f);
                CHECK_NEAR(l.entities[0].transform.rotation.w, 1.0f);
            }
            CHECK_NEAR(l.clearColor.x, 0.5f);
            CHECK(l.pointLights.empty());
            CHECK_NEAR(l.fog.density, 0.0f);
        }
        fs::remove(path);
    }

    // --- Test 5: save emits nested "transform" (no top-level position/rotation/scale) ---
    {
        SceneFile s;
        SceneEntity e;
        e.name = "x";
        e.transform.position = {1.0f, 2.0f, 3.0f};
        e.transform.scale    = {4.0f, 5.0f, 6.0f};
        e.mesh.primitive = PrimitiveKind::Cube;
        s.entities.push_back(e);

        const iron::Reflection r = makeReflectionRegistry();
        const std::string path = tempScenePath("iron_scene_nested.json");
        CHECK(saveSceneFile(r, s, path));

        std::ifstream f(path);
        std::string contents((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        f.close();
        CHECK(contents.find("\"transform\"")  != std::string::npos);
        const size_t firstName  = contents.find("\"name\"");
        const size_t firstTrans = contents.find("\"transform\"");
        CHECK(firstName  != std::string::npos);
        CHECK(firstTrans != std::string::npos);
        // nlohmann::json's default backing is std::map (alphabetical), so
        // "name" < "transform" lexicographically — invariant holds regardless
        // of insertion order. Swap-to-ordered_json would invalidate this.
        CHECK(firstTrans > firstName);
        fs::remove(path);
    }

    // --- M42: collision + audio optionals round-trip ---
    {
        iron::SceneFile s;
        iron::SceneEntity withBoth;
        withBoth.name = "crate";
        withBoth.mesh.primitive = iron::PrimitiveKind::Cube;
        withBoth.collision = iron::CollisionShape{};
        withBoth.collision->body = iron::ColliderBody::Dynamic;
        withBoth.collision->mass = 7.0f;
        withBoth.audio = iron::AudioEmitter{};
        withBoth.audio->wavPath = "hum.wav";
        withBoth.audio->loop = true;
        s.entities.push_back(withBoth);

        iron::SceneEntity plain;
        plain.name = "floor";
        plain.mesh.primitive = iron::PrimitiveKind::Plane;
        s.entities.push_back(plain);

        const iron::Reflection r = makeReflectionRegistry();
        const std::string path = tempScenePath("m42_sceneio_tmp.json");
        CHECK(iron::saveSceneFile(r, s, path));
        const auto loaded = iron::loadSceneFile(r, path);
        CHECK(loaded.has_value());
        CHECK(loaded->entities.size() == 2u);

        const auto& a = loaded->entities[0];
        CHECK(a.collision.has_value());
        CHECK(a.collision->body == iron::ColliderBody::Dynamic);
        CHECK_NEAR(a.collision->mass, 7.0f);
        CHECK(a.audio.has_value());
        CHECK(a.audio->wavPath == "hum.wav");

        const auto& b = loaded->entities[1];
        CHECK(!b.collision.has_value());
        CHECK(!b.audio.has_value());

        fs::remove(path);
    }

    return iron_test_result();
}
