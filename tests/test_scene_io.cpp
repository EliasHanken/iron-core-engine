#include "scene/SceneFormat.h"
#include "scene/SceneIO.h"
#include "math/Quaternion.h"
#include "math/Vec.h"
#include "reflection/Reflection.h"
#include "reflection/RegisterCoreTypes.h"
#include "world/CollisionShape.h"
#include "world/ComponentRegistry.h"
#include "scene/RegisterCoreComponents.h"
#include "audio/AudioEmitter.h"
#include "gameplay/LogicGraphComponent.h"
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
    iron::registerLogicGraphComponent(r);
    iron::registerHealth(r);
    return r;
}

static iron::ComponentRegistry makeComponentRegistry(const iron::Reflection& r) {
    iron::ComponentRegistry cr;
    iron::registerCoreComponents(cr, r);
    return cr;
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
        const iron::ComponentRegistry cr = makeComponentRegistry(r);
        CHECK(saveSceneFile(r, cr, original, path));

        const auto loadedOpt = loadSceneFile(r, cr, path);
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
        const iron::ComponentRegistry cr = makeComponentRegistry(r);
        const auto loaded = loadSceneFile(r, cr, path);
        CHECK(!loaded.has_value());
        fs::remove(path);
    }

    // --- Test 3: missing file returns nullopt ---
    {
        const iron::Reflection r = makeReflectionRegistry();
        const iron::ComponentRegistry cr = makeComponentRegistry(r);
        const auto loaded = loadSceneFile(r, cr, "does/not/exist/scene.json");
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
        const iron::ComponentRegistry cr = makeComponentRegistry(r);
        const auto loadedOpt = loadSceneFile(r, cr, path);
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
        const iron::ComponentRegistry cr = makeComponentRegistry(r);
        const std::string path = tempScenePath("iron_scene_nested.json");
        CHECK(saveSceneFile(r, cr, s, path));

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

    // M67: generic component round-trip via the registry.
    {
        iron::Reflection r = makeReflectionRegistry();
        iron::ComponentRegistry cr = makeComponentRegistry(r);

        iron::SceneFile scene;
        iron::SceneEntity e;
        e.name = "thing";
        iron::CollisionShape cs{};
        cs.shape = iron::ColliderShape::Sphere;
        cs.halfExtents = {1,1,1};
        cs.radius = 2.0f;
        cs.halfHeight = 0.5f;
        cs.mass = 3.0f;
        e.components.add<iron::CollisionShape>(cs);
        iron::AudioEmitter ae{};
        ae.wavPath = "boom.wav";
        ae.gain = 0.8f;
        ae.loop = true;
        ae.spatial = true;
        ae.playOnStart = false;
        e.components.add<iron::AudioEmitter>(ae);
        e.components.add<iron::LogicGraphComponent>(iron::LogicGraphComponent{ "{\"nodes\":[]}" });
        scene.entities.push_back(e);

        const std::string json = iron::sceneToJsonString(r, cr, scene);
        const iron::SceneFile back = iron::sceneFromJsonString(r, cr, json).value();

        CHECK(back.entities.size() == 1u);
        const iron::SceneEntity& b = back.entities[0];
        CHECK(b.components.has<iron::CollisionShape>());
        CHECK(b.components.get<iron::CollisionShape>()->shape == iron::ColliderShape::Sphere);
        CHECK_NEAR(b.components.get<iron::CollisionShape>()->mass, 3.0f);
        CHECK(b.components.has<iron::AudioEmitter>());
        CHECK(b.components.get<iron::AudioEmitter>()->wavPath == "boom.wav");
        CHECK(b.components.has<iron::LogicGraphComponent>());
        CHECK(b.components.get<iron::LogicGraphComponent>()->graph == "{\"nodes\":[]}");
        CHECK(!b.components.has<iron::ReflectionProbeDef>());
    }

    // M67: legacy back-compat — old top-level keys still load.
    {
        iron::Reflection r = makeReflectionRegistry();
        iron::ComponentRegistry cr = makeComponentRegistry(r);
        const std::string legacy = R"({"entities":[{"name":"old",
            "collision":{"shape":"box","body":"static","halfExtents":[2,2,2]},
            "logicGraph":"{\"v\":1}"}]})";
        const iron::SceneFile back = iron::sceneFromJsonString(r, cr, legacy).value();
        CHECK(back.entities.size() == 1u);
        CHECK(back.entities[0].components.has<iron::CollisionShape>());
        CHECK(back.entities[0].components.get<iron::CollisionShape>()->shape == iron::ColliderShape::Box);
        CHECK(back.entities[0].components.has<iron::LogicGraphComponent>());
        CHECK(back.entities[0].components.get<iron::LogicGraphComponent>()->graph == "{\"v\":1}");
    }

    // M67 headline: a brand-new component type round-trips with NO SceneIO edits.
    {
        struct DummyComp { int n = 0; float f = 0.0f; };
        iron::Reflection r = makeReflectionRegistry();
        r.registerType<DummyComp>("DummyComp").field("n", &DummyComp::n).field("f", &DummyComp::f);
        iron::ComponentRegistry cr = makeComponentRegistry(r);
        cr.registerComponent<DummyComp>("DummyComp", r);

        iron::SceneFile scene;
        iron::SceneEntity e; e.name = "x";
        e.components.add<DummyComp>(DummyComp{ 11, 2.5f });
        scene.entities.push_back(e);

        const iron::SceneFile back =
            iron::sceneFromJsonString(r, cr, iron::sceneToJsonString(r, cr, scene)).value();
        CHECK(back.entities[0].components.get<DummyComp>() != nullptr);
        CHECK(back.entities[0].components.get<DummyComp>()->n == 11);
        CHECK_NEAR(back.entities[0].components.get<DummyComp>()->f, 2.5f);
    }

    // M69: parentIndex round-trip — entity[1].parentIndex = 0 survives serialize/parse.
    {
        const iron::Reflection r = makeReflectionRegistry();
        const iron::ComponentRegistry cr = makeComponentRegistry(r);
        iron::SceneFile s;
        iron::SceneEntity a; a.name = "root";
        iron::SceneEntity b; b.name = "child"; b.parentIndex = 0;
        s.entities = {a, b};
        const std::string json = iron::sceneToJsonString(r, cr, s);
        const auto loadedOpt = iron::sceneFromJsonString(r, cr, json);
        CHECK(loadedOpt.has_value());
        if (loadedOpt.has_value()) {
            CHECK(loadedOpt->entities.size() == 2u);
            CHECK(loadedOpt->entities[0].parentIndex == -1);
            CHECK(loadedOpt->entities[1].parentIndex == 0);
        }
    }

    // M69: legacy files with no "parent" key default to -1.
    {
        const iron::Reflection r = makeReflectionRegistry();
        const iron::ComponentRegistry cr = makeComponentRegistry(r);
        const char* legacy = R"({"entities":[{"name":"a"},{"name":"b"}]})";
        const auto loadedOpt = iron::sceneFromJsonString(r, cr, legacy);
        CHECK(loadedOpt.has_value());
        if (loadedOpt.has_value()) {
            CHECK(loadedOpt->entities.size() == 2u);
            CHECK(loadedOpt->entities[0].parentIndex == -1);
            CHECK(loadedOpt->entities[1].parentIndex == -1);
        }
    }

    // M69: out-of-range parent is sanitized to -1 at load time.
    {
        const iron::Reflection r = makeReflectionRegistry();
        const iron::ComponentRegistry cr = makeComponentRegistry(r);
        const char* bad = R"({"entities":[{"name":"a","parent":7}]})";
        const auto loadedOpt = iron::sceneFromJsonString(r, cr, bad);
        CHECK(loadedOpt.has_value());
        if (loadedOpt.has_value()) {
            CHECK(loadedOpt->entities.size() == 1u);
            CHECK(loadedOpt->entities[0].parentIndex == -1);
        }
    }

    // M69: a cyclic parent chain is broken at load time (cycle guard, pass 2).
    {
        const iron::Reflection r = makeReflectionRegistry();
        const iron::ComponentRegistry cr = makeComponentRegistry(r);
        // a->parent=1, b->parent=0 : a 2-node cycle.
        const char* cyclic = R"({"entities":[{"name":"a","parent":1},{"name":"b","parent":0}]})";
        const auto loadedOpt = iron::sceneFromJsonString(r, cr, cyclic);
        CHECK(loadedOpt.has_value());
        if (loadedOpt.has_value()) {
            CHECK(loadedOpt->entities.size() == 2u);
            // At least one link must be reset to -1 so no cycle remains.
            CHECK(loadedOpt->entities[0].parentIndex == -1 ||
                  loadedOpt->entities[1].parentIndex == -1);
        }
    }

    // In-memory string round-trip (M57): toJsonString -> fromJsonString
    // preserves entity name + transform + a material factor.
    {
        const Reflection r = makeReflectionRegistry();
        const iron::ComponentRegistry cr = makeComponentRegistry(r);
        SceneFile s;
        SceneEntity e;
        e.name = "undo_probe";
        e.transform.position = {1.0f, 2.0f, 3.0f};
        e.material.metallic = 0.25f;
        s.entities.push_back(e);

        const std::string json = sceneToJsonString(r, cr, s);
        CHECK(!json.empty());
        auto back = sceneFromJsonString(r, cr, json);
        CHECK(back.has_value());
        CHECK(back->entities.size() == 1u);
        CHECK(back->entities[0].name == "undo_probe");
        CHECK_NEAR(back->entities[0].transform.position.x, 1.0f);
        CHECK_NEAR(back->entities[0].transform.position.y, 2.0f);
        CHECK_NEAR(back->entities[0].transform.position.z, 3.0f);
        CHECK_NEAR(back->entities[0].material.metallic, 0.25f);

        // Malformed input -> nullopt.
        CHECK(!sceneFromJsonString(r, cr, "{ not json").has_value());
    }

    return iron_test_result();
}
