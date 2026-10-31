// Tests for Forge: reading documents, helpful errors, and generated shaders.
#include "forge/Forge.h"
#include "forge/ForgeEdit.h"
#include "forge/ForgePlayer.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

using nlohmann::json;
using namespace iron::forge;

namespace {

Document parse(const char* text, std::vector<Issue>& issues) {
    return parseDocument(json::parse(text), issues);
}

const Issue* findCode(const std::vector<Issue>& issues, const std::string& code) {
    const auto it = std::find_if(issues.begin(), issues.end(),
                                 [&](const Issue& i) { return i.code == code; });
    return it == issues.end() ? nullptr : &*it;
}

const char* const kSimple = R"({
  "target": "particles", "count": 1000,
  "spawn": [{"block": "Sphere", "radius": 3}],
  "move": [{"block": "CurlFlow", "strength": 2.5}],
  "look": [{"block": "Color", "color": "#ff8000"}, {"block": "Glow"}]
})";

void testSingleLayerParses() {
    std::vector<Issue> issues;
    const Document doc = parse(kSimple, issues);
    assert(issues.empty());
    assert(doc.layers.size() == 1);
    assert(doc.layers[0].count == 1000);
    assert(doc.layers[0].spawn[0].values.at("radius").x == 3.0f);
}

void testHexColorsBecomeLinear() {
    std::vector<Issue> issues;
    const Document doc = parse(kSimple, issues);
    const iron::Vec3 c = doc.layers[0].look[0].values.at("color");
    assert(c.x == 1.0f && c.z == 0.0f);
    assert(c.y > 0.1f && c.y < 0.4f);  // 0x80 is mid in sRGB, darker in linear.
}

void testMisspeltBlockSuggestsTheRightOne() {
    std::vector<Issue> issues;
    parse(R"({"move": [{"block": "CurlFlw"}]})", issues);
    const Issue* issue = findCode(issues, "unknown-block");
    assert(issue);
    assert(issue->where == "layers.0.move.0");
    assert(issue->fix.find("CurlFlow") != std::string::npos);
}

void testMisspeltValueSuggestsTheRightOne() {
    std::vector<Issue> issues;
    parse(R"({"move": [{"block": "CurlFlow", "strenght": 2}]})", issues);
    const Issue* issue = findCode(issues, "unknown-value");
    assert(issue);
    assert(issue->fix.find("strength") != std::string::npos);
}

void testBlockInTheWrongStackSaysWhereItGoes() {
    std::vector<Issue> issues;
    parse(R"({"look": [{"block": "CurlFlow"}]})", issues);
    const Issue* issue = findCode(issues, "wrong-stack");
    assert(issue);
    assert(issue->fix.find("move") != std::string::npos);
}

void testMissingAndUnknownShapes() {
    std::vector<Issue> issues;
    parse(R"({"move": [{"block": "StickToShape"}]})", issues);
    assert(findCode(issues, "needs-shape"));

    issues.clear();
    parse(R"({"shapes": {"orb": {"type": "Sphere"}},
              "move": [{"block": "StickToShape", "shape": "orbb"}]})", issues);
    const Issue* issue = findCode(issues, "unknown-shape");
    assert(issue);
    assert(issue->fix.find("orb") != std::string::npos);
}

void testIssuesAreMachineReadable() {
    std::vector<Issue> issues;
    parse(R"({"move": [{"block": "Nope"}]})", issues);
    const json out = issuesToJson(issues);
    assert(out.is_array() && !out.empty());
    assert(out[0].contains("where") && out[0].contains("code") && out[0].contains("message"));
}

void testDocumentRoundTrips() {
    std::vector<Issue> issues;
    const Document first = parse(kSimple, issues);
    const json written = documentToJson(first);
    const Document second = parseDocument(written, issues);
    assert(issues.empty());
    assert(documentToJson(second) == written);
}

void testValuesChangeSlotsNotShaders() {
    std::vector<Issue> issues;
    Document doc = parse(kSimple, issues);
    const Compiled before = compile(doc);
    assert(before.ok());

    doc.layers[0].move[0].values["strength"].x = 9.0f;
    const Compiled after = compile(doc);
    assert(after.ok());

    assert(before.layers[0].simulateGlsl == after.layers[0].simulateGlsl);
    assert(before.layers[0].slots != after.layers[0].slots);
}

void testWiresDriveAValue() {
    std::vector<Issue> issues;
    const Document doc = parse(R"({
      "nodes": {"pulse": {"type": "Sine", "speed": 2}},
      "wires": [{"from": "pulse.out", "to": "move.0.strength"}],
      "spawn": [{"block": "Sphere"}], "move": [{"block": "CurlFlow"}], "look": [{"block": "Glow"}]
    })", issues);
    assert(issues.empty());
    const Compiled compiled = compile(doc);
    assert(compiled.ok());
    assert(compiled.layers[0].simulateGlsl.find("node_pulse") != std::string::npos);
    assert(checkShaders(compiled).empty());
}

void testWireFromNowhereIsAnError() {
    std::vector<Issue> issues;
    parse(R"({"wires": [{"from": "ghost.out", "to": "move.0.strength"}]})", issues);
    assert(findCode(issues, "unknown-wire-source"));
}

void testWireLoopIsAnError() {
    std::vector<Issue> issues;
    parse(R"({
      "nodes": {"a": {"type": "Add"}, "b": {"type": "Add"}},
      "wires": [{"from": "a", "to": "b.a"}, {"from": "b", "to": "a.a"},
                {"from": "a", "to": "move.0.strength"}],
      "spawn": [{"block": "Sphere"}], "move": [{"block": "CurlFlow"}], "look": [{"block": "Glow"}]
    })", issues);
    assert(findCode(issues, "wire-loop"));
}

void testEveryBlockCompilesFromItsDefaults() {
    for (const BlockDesc& block : blocks()) {
        if (block.stack == Stack::Surface) continue;
        Document doc;
        doc.shapes["s"] = ShapeInstance{"Sphere", {}};
        Layer layer;
        layer.count = 16;
        BlockInstance instance;
        instance.block = block.name;
        if (block.needsShape) instance.shape = "s";
        if (block.stack == Stack::Spawn) layer.spawn.push_back(instance);
        if (block.stack == Stack::Move) layer.move.push_back(instance);
        if (block.stack == Stack::Look) layer.look.push_back(instance);
        doc.layers.push_back(layer);

        const Compiled compiled = compile(doc);
        assert(compiled.ok());
        const std::vector<Issue> shaderIssues = checkShaders(compiled);
        if (!shaderIssues.empty()) std::printf("block failed: %s\n", block.name.c_str());
        assert(shaderIssues.empty());
    }
}

void testEveryShapeAndNodeCompiles() {
    for (const ShapeDesc& shape : shapes()) {
        Document doc;
        doc.shapes["s"] = ShapeInstance{shape.name, {}};
        Layer layer;
        layer.count = 16;
        BlockInstance stick;
        stick.block = "StickToShape";
        stick.shape = "s";
        layer.move.push_back(stick);
        doc.layers.push_back(layer);
        assert(checkShaders(compile(doc)).empty());
    }
    for (const NodeDesc& node : nodes()) {
        Document doc;
        doc.nodes["n"] = NodeInstance{node.name, {}};
        Layer layer;
        layer.count = 16;
        BlockInstance curl;
        curl.block = "CurlFlow";
        layer.move.push_back(curl);
        doc.layers.push_back(layer);
        doc.wires.push_back({"n", "layers.0.move.0.strength"});
        const Compiled compiled = compile(doc);
        if (node.output != ValueType::Float) continue;
        assert(compiled.ok());
        assert(checkShaders(compiled).empty());
    }
}

void testEverySurfaceBlockCompiles() {
    for (const BlockDesc& block : blocks()) {
        if (block.stack != Stack::Surface) continue;
        Document doc;
        doc.target = "surface";
        BlockInstance instance;
        instance.block = block.name;
        doc.surface.push_back(instance);
        const Compiled compiled = compile(doc);
        assert(compiled.ok() && compiled.surface);
        if (!checkShaders(compiled).empty()) std::printf("block failed: %s\n", block.name.c_str());
        assert(checkShaders(compiled).empty());
    }
}

void testCatalogDescribesEverything() {
    const json catalog = catalogJson();
    assert(catalog["blocks"].size() == blocks().size());
    for (const json& block : catalog["blocks"]) {
        assert(!block["summary"].get<std::string>().empty());
        assert(block.contains("stack") && block.contains("params"));
    }
    assert(catalog["shapes"].size() == shapes().size());
    assert(catalog["nodes"].size() == nodes().size());
}

void testOrbExampleIsValid() {
    std::vector<Issue> issues;
    const Document doc = loadDocument(IRON_REPO_ROOT "/docs/forge/examples/orb.forge.json", issues);
    assert(issues.empty());
    assert(doc.layers.size() == 2);
    const Compiled compiled = compile(doc);
    assert(compiled.ok());
    assert(checkShaders(compiled).empty());
}

void testEveryTemplateWorks() {
    for (const std::string& name : templateNames()) {
        const Document doc = makeTemplate(name);
        const Compiled compiled = compile(doc);
        assert(compiled.ok());
        assert(checkShaders(compiled).empty());
    }
}

void testAddingAShapeBlockCreatesAShape() {
    Document doc = makeTemplate("Sparks");
    assert(doc.shapes.empty());
    addBlock(doc, doc.layers[0], Stack::Move, "StickToShape");
    assert(doc.shapes.size() == 1);
    assert(compile(doc).ok());
}

void testAnyBlockCanBeAddedAndStillCompile() {
    for (const BlockDesc& block : blocks()) {
        if (block.stack == Stack::Surface) continue;
        Document doc = makeTemplate("Sparks");
        addBlock(doc, doc.layers[0], block.stack, block.name);
        assert(compile(doc).ok());
    }
}

void testRemovingABlockKeepsWiresOnTheirTargets() {
    Document doc = makeTemplate("Sparks");
    const std::string path = valuePath(0, Stack::Move, 2, "amount");
    driveValue(doc, path, "Sine");

    removeBlock(doc, 0, Stack::Move, 0);
    assert(driverOf(doc, valuePath(0, Stack::Move, 1, "amount")) == "drive1");

    removeBlock(doc, 0, Stack::Move, 1);
    assert(doc.wires.empty());
}

void testMovingABlockKeepsWiresOnTheirTargets() {
    Document doc = makeTemplate("Sparks");
    driveValue(doc, valuePath(0, Stack::Move, 2, "amount"), "Sine");
    moveBlock(doc, 0, Stack::Move, 2, -1);
    assert(doc.layers[0].move[1].block == "Drag");
    assert(driverOf(doc, valuePath(0, Stack::Move, 1, "amount")) == "drive1");
    assert(compile(doc).ok());
}

void testDrivingAValueAndStopping() {
    Document doc = makeTemplate("Sparks");
    const std::string path = valuePath(0, Stack::Move, 2, "amount");
    const std::string node = driveValue(doc, path, "Noise");
    assert(doc.nodes.count(node) == 1);

    const Compiled compiled = compile(doc);
    assert(compiled.ok());
    assert(compiled.layers[0].simulateGlsl.find("node_" + node) != std::string::npos);

    undriveValue(doc, path);
    assert(doc.nodes.empty() && doc.wires.empty());
}

void testRibbonsCompile() {
    std::vector<Issue> issues;
    const Document doc = parse(R"({"count": 100, "trail": 32,
      "spawn": [{"block": "Sphere"}], "move": [{"block": "CurlFlow"}],
      "look": [{"block": "Color"}, {"block": "Size", "size": 0.03}]})", issues);
    assert(issues.empty());
    assert(doc.layers[0].trail == 32);
    assert(checkShaders(compile(doc)).empty());
}

void testTrailLengthIsChecked() {
    std::vector<Issue> issues;
    parse(R"({"trail": 500})", issues);
    assert(findCode(issues, "bad-trail"));
}

// Every one of these is valid JSON with a wrong type, and none may crash.
void testWrongTypesBecomeIssues() {
    for (const char* text : {R"({"count": "200000"})", R"({"layers": [3]})",
                             R"({"nodes": {"a": 3}})", R"({"wires": [1]})",
                             R"({"shapes": {"a": 3}})", R"([1, 2])"}) {
        std::vector<Issue> issues;
        parse(text, issues);
        assert(!issues.empty());
    }
}

void testHugeAndNegativeCountsAreRefused() {
    std::vector<Issue> issues;
    parse(R"({"count": -1})", issues);
    assert(findCode(issues, "bad-count"));

    issues.clear();
    parse(R"({"count": 4000000000})", issues);
    assert(findCode(issues, "bad-count"));

    issues.clear();
    parse(R"({"count": 1500000, "trail": 64})", issues);
    assert(findCode(issues, "trail-too-large"));
}

void testTrailMustBeARealRibbon() {
    std::vector<Issue> issues;
    parse(R"({"trail": 1})", issues);
    assert(findCode(issues, "bad-trail"));

    issues.clear();
    parse(R"({"trail": 8, "blockLight": true})", issues);
    assert(findCode(issues, "smoke-ribbon"));
}

void testWireTargetsAreChecked() {
    const char* const layer = R"("spawn": [{"block": "Sphere"}], "move": [{"block": "CurlFlow"}])";
    const auto check = [&](const std::string& to) {
        const std::string text = std::string(R"({"nodes": {"pulse": {"type": "Sine"}},
          "wires": [{"from": "pulse", "to": ")") + to + R"("}], )" + layer + "}";
        std::vector<Issue> issues;
        parse(text.c_str(), issues);
        return issues;
    };
    const std::vector<Issue> typo = check("layers.0.move.0.strenght");
    assert(findCode(typo, "unknown-wire-target"));
    assert(findCode(typo, "unknown-wire-target")->fix.find("strength") != std::string::npos);
    assert(findCode(check("layers.7.move.0.strength"), "unknown-wire-target"));
    assert(findCode(check("shapes.orb.radius"), "unknown-wire-target"));
    assert(check("layers.0.move.0.strength").empty());
    assert(check("move.0.strength").empty());
    assert(check("pulse.speed").empty() == false);  // A node can't drive itself.
}

void testBareWireMeansTheFirstLayerOnly() {
    std::vector<Issue> issues;
    const Document doc = parse(R"({
      "nodes": {"pulse": {"type": "Sine"}},
      "wires": [{"from": "pulse.out", "to": "move.0.strength"}],
      "layers": [{"move": [{"block": "CurlFlow"}]}, {"move": [{"block": "CurlFlow"}]}]
    })", issues);
    assert(issues.empty());
    assert(doc.wires[0].to == "layers.0.move.0.strength");
    assert(driverOf(doc, valuePath(0, Stack::Move, 0, "strength")) == "pulse");

    const Compiled compiled = compile(doc);
    assert(compiled.layers[0].simulateGlsl.find("node_pulse") != std::string::npos);
    assert(compiled.layers[1].simulateGlsl.find("node_pulse") == std::string::npos);
}

void testStoppingOneDriverKeepsASharedNode() {
    std::vector<Issue> issues;
    Document doc = parse(R"({
      "nodes": {"pulse": {"type": "Sine"}},
      "wires": [{"from": "pulse.out", "to": "move.0.strength"},
                {"from": "pulse.out", "to": "move.0.scale"}],
      "move": [{"block": "CurlFlow"}]
    })", issues);
    assert(issues.empty());
    undriveValue(doc, "layers.0.move.0.strength");
    assert(doc.nodes.count("pulse") == 1);
    assert(compile(doc).ok());
}

void testNamesMustBeIdentifiers() {
    std::vector<Issue> issues;
    parse(R"({"nodes": {"my-pulse": {"type": "Sine"}}})", issues);
    assert(findCode(issues, "bad-name"));
}

void testOneLightPerLayer() {
    std::vector<Issue> issues;
    parse(R"({"look": [{"block": "LightTheScene"}, {"block": "LightTheScene"}]})", issues);
    assert(findCode(issues, "two-lights"));
}

void testLightBlockMakesALayerEmit() {
    std::vector<Issue> issues;
    const Document doc = parse(R"({"look": [{"block": "LightTheScene"}]})", issues);
    assert(issues.empty());
    const Compiled compiled = compile(doc);
    assert(compiled.layers[0].emitsLight);
    assert(checkShaders(compiled).empty());
}

void testNearestName() {
    assert(nearestName("Sphre", {"Sphere", "Box"}) == "Sphere");
    assert(nearestName("zzzzzzzz", {"Sphere", "Box"}).empty());
}

}  // namespace

int main() {
    testSingleLayerParses();
    testHexColorsBecomeLinear();
    testMisspeltBlockSuggestsTheRightOne();
    testMisspeltValueSuggestsTheRightOne();
    testBlockInTheWrongStackSaysWhereItGoes();
    testMissingAndUnknownShapes();
    testIssuesAreMachineReadable();
    testDocumentRoundTrips();
    testValuesChangeSlotsNotShaders();
    testWiresDriveAValue();
    testWireFromNowhereIsAnError();
    testWireLoopIsAnError();
    testEveryBlockCompilesFromItsDefaults();
    testEveryShapeAndNodeCompiles();
    testEverySurfaceBlockCompiles();
    testCatalogDescribesEverything();
    testOrbExampleIsValid();
    testEveryTemplateWorks();
    testAddingAShapeBlockCreatesAShape();
    testAnyBlockCanBeAddedAndStillCompile();
    testRemovingABlockKeepsWiresOnTheirTargets();
    testMovingABlockKeepsWiresOnTheirTargets();
    testDrivingAValueAndStopping();
    testRibbonsCompile();
    testTrailLengthIsChecked();
    testWrongTypesBecomeIssues();
    testHugeAndNegativeCountsAreRefused();
    testTrailMustBeARealRibbon();
    testWireTargetsAreChecked();
    testBareWireMeansTheFirstLayerOnly();
    testStoppingOneDriverKeepsASharedNode();
    testNamesMustBeIdentifiers();
    testOneLightPerLayer();
    testLightBlockMakesALayerEmit();
    testNearestName();
    std::puts("test_forge: all passed");
    return 0;
}
