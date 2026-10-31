// ForgeCatalog.cpp — every block, node and shape Forge offers. Adding one here is
// all it takes: the compiler, checker, catalog and editor are driven by this data.

#include "forge/Forge.h"

namespace iron::forge {

namespace {

ParamDesc num(std::string name, float value, float min, float max, std::string description) {
    return {std::move(name), ValueType::Float, Vec3{value, 0, 0}, min, max, std::move(description)};
}

ParamDesc color(std::string name, Vec3 value, std::string description) {
    return {std::move(name), ValueType::Color, value, 0.0f, 1.0f, std::move(description)};
}

ParamDesc vec(std::string name, Vec3 value, float min, float max, std::string description) {
    return {std::move(name), ValueType::Vec3, value, min, max, std::move(description)};
}

std::vector<BlockDesc> makeBlocks() {
    std::vector<BlockDesc> list;

    // --- Spawn: pos, vel, life, keep ------------------------------------------------
    list.push_back({"Sphere", Stack::Spawn, "Start in or on a sphere.",
        {num("radius", 2.0f, 0.0f, 30.0f, "Size in metres."),
         num("shell", 1.0f, 0.0f, 1.0f, "1 starts on the surface, 0 fills the inside."),
         vec("center", Vec3{0, 0, 0}, -30.0f, 30.0f, "Middle of the sphere.")},
        R"(pos = $center + randomDirection(seed) * $radius * mix(pow(rnd(seed + 7u), 0.3333), 1.0, $shell);)"});
    list.push_back({"Point", Stack::Spawn, "Start at one point.",
        {vec("position", Vec3{0, 0, 0}, -30.0f, 30.0f, "Where particles start."),
         num("spread", 0.0f, 0.0f, 5.0f, "Random offset in metres.")},
        R"(pos = $position + randomDirection(seed) * $spread * rnd(seed + 7u);)"});
    list.push_back({"OnShape", Stack::Spawn, "Start on the surface of a shape.",
        {num("reach", 6.0f, 0.1f, 40.0f, "How far out to look for the surface.")},
        R"({
    vec3 q = (vec3(rnd(seed), rnd(seed + 1u), rnd(seed + 2u)) - 0.5) * 2.0 * $reach;
    for (int i = 0; i < 6; ++i) q -= shapeNormal(q) * shapeDistance(q);
    pos = q;
})", true});
    list.push_back({"Lifetime", Stack::Spawn, "How long each particle lives.",
        {num("min", 1.0f, 0.05f, 20.0f, "Shortest life in seconds."),
         num("max", 3.0f, 0.05f, 20.0f, "Longest life in seconds.")},
        R"(life = mix($min, $max, rnd(seed + 11u));)"});
    list.push_back({"Launch", Stack::Spawn, "Start moving outward from the middle.",
        {num("speed", 2.0f, 0.0f, 40.0f, "Metres per second."),
         num("spread", 0.0f, 0.0f, 1.0f, "How much of that speed is random.")},
        R"(vel = normalize(pos + vec3(1e-4)) * $speed * (1.0 - $spread * rnd(seed + 13u));)"});
    list.push_back({"Amount", Stack::Spawn, "How much of the effect is active.",
        {num("amount", 1.0f, 0.0f, 1.0f, "0 stops new particles, 1 runs all of them.")},
        R"(keep = keep && rnd(seed + 17u) <= $amount;)"});

    // --- Move: x, v, dt, life01, seedValue ------------------------------------------
    list.push_back({"CurlFlow", Stack::Move, "Swirl along a smooth flow, like smoke in air.",
        {num("strength", 2.0f, 0.0f, 20.0f, "How hard the flow pushes."),
         num("scale", 0.6f, 0.02f, 4.0f, "Size of the swirls: higher is finer."),
         num("speed", 0.2f, 0.0f, 3.0f, "How quickly the flow changes.")},
        R"(v += curlNoise(x * $scale + vec3(0.0, frame.time * $speed, 0.0)) * $strength * dt * 6.0;)"});
    list.push_back({"Gravity", Stack::Move, "Constant push in one direction.",
        {vec("force", Vec3{0, -2, 0}, -20.0f, 20.0f, "Metres per second squared.")},
        R"(v += $force * dt;)"});
    list.push_back({"Attract", Stack::Move, "Pull toward a point.",
        {vec("point", Vec3{0, 0, 0}, -30.0f, 30.0f, "Where to pull."),
         num("strength", 3.0f, -30.0f, 30.0f, "Negative pushes away.")},
        R"(v += normalize($point - x + vec3(1e-4)) * $strength * dt;)"});
    list.push_back({"Swirl", Stack::Move, "Circle around an axis.",
        {vec("axis", Vec3{0, 1, 0}, -1.0f, 1.0f, "Axis through the middle."),
         num("strength", 3.0f, -30.0f, 30.0f, "Speed of the spin.")},
        R"(v += cross(normalize($axis + vec3(1e-4)), normalize(x + vec3(1e-4))) * $strength * dt;)"});
    list.push_back({"Drag", Stack::Move, "Slow down over time.",
        {num("amount", 1.0f, 0.0f, 12.0f, "Higher stops sooner.")},
        R"(v *= exp(-$amount * dt);)"});
    list.push_back({"StickToShape", Stack::Move, "Stay on a shape's surface and flow around it.",
        {num("grip", 8.0f, 0.0f, 30.0f, "How tightly they hold on."),
         num("flow", 3.2f, 0.0f, 20.0f, "How fast they run along the surface."),
         num("scale", 0.75f, 0.02f, 4.0f, "Size of the flow pattern."),
         num("speed", 0.22f, 0.0f, 3.0f, "How quickly the pattern changes.")},
        R"({
    float d = shapeDistance(x);
    vec3 n = shapeNormal(x);
    vec3 flow = curlNoise(x * $scale + vec3(0.0, frame.time * $speed, 0.0));
    vec3 along = flow - n * dot(flow, n);
    vec3 want = along * $flow - n * clamp(d, -1.0, 1.0) * $grip;
    v = mix(v, want, 1.0 - exp(-dt * 9.0));
    normalOut = n;
})", true});
    list.push_back({"Bounce", Stack::Move, "Collide with a shape and bounce off.",
        {num("bounce", 0.4f, 0.0f, 1.0f, "1 keeps all speed, 0 stops dead."),
         num("friction", 0.1f, 0.0f, 1.0f, "Speed lost along the surface.")},
        R"({
    float d = shapeDistance(x);
    if (d < 0.0) {
        vec3 n = shapeNormal(x);
        x -= n * d;
        float into = dot(v, n);
        if (into < 0.0) {
            vec3 slide = (v - n * into) * (1.0 - $friction);
            v = slide - n * into * $bounce;
        }
    }
})", true});

    // --- Look: tint, size, alpha, stretch, life01, x, v, n ----------------------------
    list.push_back({"Color", Stack::Look, "One colour.",
        {color("color", Vec3{1.0f, 0.5f, 0.2f}, "Linear colour.")},
        R"(tint = $color;)"});
    list.push_back({"ColorOverLife", Stack::Look, "Fade from one colour to another.",
        {color("start", Vec3{1.0f, 0.85f, 0.5f}, "Colour when born."),
         color("end", Vec3{1.0f, 0.1f, 0.02f}, "Colour when dying.")},
        R"(tint = mix($start, $end, life01);)"});
    list.push_back({"Size", Stack::Look, "Particle size.",
        {num("size", 0.02f, 0.001f, 2.0f, "Half-width in metres."),
         num("vary", 0.5f, 0.0f, 1.0f, "Random variation.")},
        R"(size = $size * (1.0 + $vary * (seedValue - 0.5));)"});
    list.push_back({"SizeOverLife", Stack::Look, "Grow or shrink with age.",
        {num("start", 1.0f, 0.0f, 10.0f, "Multiplier when born."),
         num("end", 0.0f, 0.0f, 10.0f, "Multiplier when dying.")},
        R"(size *= mix($start, $end, life01);)"});
    list.push_back({"Fade", Stack::Look, "Fade in and out over a life.",
        {num("in", 0.1f, 0.0f, 1.0f, "Share of life spent fading in."),
         num("out", 0.3f, 0.0f, 1.0f, "Share of life spent fading out.")},
        R"(alpha *= smoothstep(0.0, max($in, 1e-3), life01) * (1.0 - smoothstep(1.0 - max($out, 1e-3), 1.0, life01));)"});
    list.push_back({"StretchWithSpeed", Stack::Look, "Draw as a streak along the motion.",
        {num("amount", 0.12f, 0.0f, 2.0f, "Seconds of motion to smear."),
         num("max", 0.3f, 0.0f, 5.0f, "Longest streak in metres.")},
        R"(stretch = $amount; maxStretch = $max;)"});
    list.push_back({"Glow", Stack::Look, "How much light each particle gives.",
        {num("intensity", 0.4f, 0.0f, 20.0f, "Above 1 starts to bloom when many overlap.")},
        R"(alpha *= $intensity;)"});
    list.push_back({"Rim", Stack::Look, "Brighter at the edges of a shape, darker behind it.",
        {num("edge", 0.5f, 0.0f, 3.0f, "Extra brightness at the silhouette."),
         num("back", 0.1f, 0.0f, 1.0f, "Brightness of the far side.")},
        R"({
    vec3 toCamera = normalize(frame.cameraPos.xyz - x);
    float facing = dot(n, toCamera);
    alpha *= mix($back, 1.0, smoothstep(-0.15, 0.2, facing)) + $edge * pow(1.0 - abs(facing), 3.0);
})"});
    list.push_back({"Strokes", Stack::Look, "Bands of brightness that crawl over the effect.",
        {num("scale", 3.4f, 0.1f, 20.0f, "Size of the bands: higher is finer."),
         num("sharpness", 7.0f, 1.0f, 16.0f, "Higher is thinner and hotter."),
         num("base", 0.05f, 0.0f, 1.0f, "Brightness between bands."),
         color("hot", Vec3{0.7f, 0.9f, 1.0f}, "Colour of the bands."),
         num("speed", 0.5f, 0.0f, 4.0f, "How quickly they crawl.")},
        R"({
    float ridge = 1.0 - abs(2.0 * valueNoise(x * $scale + vec3(0.0, frame.time * $speed, 0.0)) - 1.0);
    float band = pow(ridge, $sharpness);
    tint = mix(tint, $hot, band);
    alpha *= $base + band;
})"});
    list.push_back({"LightTheScene", Stack::Look, "Add this layer's glow to the air and surfaces around it.",
        {color("color", Vec3{1.0f, 0.5f, 0.2f}, "Colour of the light."),
         num("amount", 0.01f, 0.0f, 1.0f, "Light per particle.")},
        "", false, R"(return $color * $amount;)"});

    // --- Surface: albedo, roughness, metallic, emissive, worldPos, N, V ---------------
    list.push_back({"BaseColor", Stack::Surface, "The surface's colour.",
        {color("color", Vec3{0.5f, 0.5f, 0.5f}, "Linear colour.")},
        R"(albedo = $color;)"});
    list.push_back({"Finish", Stack::Surface, "How rough or metallic it is.",
        {num("roughness", 0.5f, 0.0f, 1.0f, "0 is a mirror, 1 is chalk."),
         num("metallic", 0.0f, 0.0f, 1.0f, "1 is bare metal.")},
        R"(roughness = $roughness; metallic = $metallic;)"});
    list.push_back({"Marble", Stack::Surface, "Veined stone between two colours.",
        {color("colorA", Vec3{0.05f, 0.05f, 0.06f}, "First colour."),
         color("colorB", Vec3{0.8f, 0.75f, 0.65f}, "Second colour."),
         num("scale", 2.0f, 0.05f, 30.0f, "Size of the pattern: higher is finer."),
         num("sharpness", 5.0f, 1.0f, 16.0f, "Higher gives thinner veins.")},
        R"({
    float ridge = pow(1.0 - abs(2.0 * fbm(worldPos * $scale, 5) - 1.0), $sharpness);
    albedo = mix($colorA, $colorB, ridge);
})"});
    list.push_back({"LavaCracks", Stack::Surface, "Glowing cracks across a dark crust.",
        {color("hot", Vec3{1.0f, 0.3f, 0.05f}, "Colour of the molten glow."),
         num("intensity", 3.0f, 0.0f, 40.0f, "Brightness of the glow."),
         num("width", 0.03f, 0.0f, 1.0f, "How wide the cracks open."),
         num("scale", 1.2f, 0.05f, 30.0f, "Size of the pattern: higher is finer."),
         num("speed", 0.1f, 0.0f, 3.0f, "How quickly it churns.")},
        R"({
    float n = fbm(worldPos * $scale + vec3(0.0, surfaceTime * $speed, 0.0), 5);
    float ridge = 1.0 - abs(2.0 * n - 1.0);
    float edge = 1.0 - $width;
    float crack = smoothstep(edge, min(edge + 0.04, 1.0), ridge);
    emissive += $hot * crack * $intensity;
    roughness = mix(roughness, 0.9, crack);
})"});
    list.push_back({"Fresnel", Stack::Surface, "Glow at grazing angles, like an energy shield.",
        {color("color", Vec3{0.3f, 0.6f, 1.0f}, "Colour of the rim."),
         num("intensity", 3.0f, 0.0f, 40.0f, "Brightness of the rim."),
         num("power", 3.0f, 0.5f, 12.0f, "Higher is a thinner rim.")},
        R"(emissive += $color * pow(1.0 - max(dot(N, V), 0.0), $power) * $intensity;)"});
    list.push_back({"Emission", Stack::Surface, "Glow with one colour.",
        {color("color", Vec3{1.0f, 0.5f, 0.2f}, "Colour of the glow."),
         num("intensity", 1.0f, 0.0f, 40.0f, "Brightness.")},
        R"(emissive += $color * $intensity;)"});

    return list;
}

std::vector<NodeDesc> makeNodes() {
    return {
        {"Number", "A fixed number.", {num("value", 1.0f, -100.0f, 100.0f, "The number.")},
         ValueType::Float, "$value"},
        {"Time", "Seconds since the effect began.", {num("speed", 1.0f, -10.0f, 10.0f, "Multiplier.")},
         ValueType::Float, "(forgeTime * $speed)"},
        {"Sine", "A smooth wave between low and high.",
         {num("speed", 1.0f, 0.0f, 30.0f, "Waves per second."),
          num("low", 0.0f, -100.0f, 100.0f, "Lowest value."),
          num("high", 1.0f, -100.0f, 100.0f, "Highest value.")},
         ValueType::Float, "mix($low, $high, 0.5 + 0.5 * sin(forgeTime * $speed * 6.2831853))"},
        {"Add", "a + b", {num("a", 0.0f, -100.0f, 100.0f, ""), num("b", 0.0f, -100.0f, 100.0f, "")},
         ValueType::Float, "($a + $b)"},
        {"Multiply", "a x b", {num("a", 1.0f, -100.0f, 100.0f, ""), num("b", 1.0f, -100.0f, 100.0f, "")},
         ValueType::Float, "($a * $b)"},
        {"Remap", "Map 0..1 onto low..high.",
         {num("value", 0.5f, 0.0f, 1.0f, "Input."), num("low", 0.0f, -100.0f, 100.0f, "Output at 0."),
          num("high", 1.0f, -100.0f, 100.0f, "Output at 1.")},
         ValueType::Float, "mix($low, $high, $value)"},
        {"Noise", "Smooth random value at a position in time.",
         {num("scale", 1.0f, 0.01f, 30.0f, "Higher is finer."),
          num("speed", 0.5f, 0.0f, 10.0f, "How quickly it changes.")},
         ValueType::Float, "gradientNoise(forgePosition * $scale + vec3(0.0, forgeTime * $speed, 0.0))"},
        {"ColorMix", "Blend two colours.",
         {color("a", Vec3{0, 0, 0}, ""), color("b", Vec3{1, 1, 1}, ""),
          num("t", 0.5f, 0.0f, 1.0f, "0 is a, 1 is b.")},
         ValueType::Color, "mix($a, $b, $t)"},
    };
}

std::vector<ShapeDesc> makeShapes() {
    return {
        {"Sphere", "A ball.",
         {num("radius", 2.0f, 0.01f, 30.0f, "Size in metres."),
          vec("center", Vec3{0, 0, 0}, -30.0f, 30.0f, "Middle.")},
         "(length(p - $center) - $radius)"},
        {"LumpySphere", "A ball with a slowly moving, uneven surface.",
         {num("radius", 2.6f, 0.01f, 30.0f, "Size in metres."),
          num("lumps", 0.14f, 0.0f, 1.0f, "How uneven."),
          num("scale", 1.8f, 0.05f, 10.0f, "Size of the lumps: higher is finer."),
          num("speed", 0.18f, 0.0f, 3.0f, "How quickly they move.")},
         "(length(p) - $radius * (1.0 + $lumps * (valueNoise(normalize(p + vec3(1e-4)) * $scale + "
         "vec3(0.0, frame.time * $speed, 0.0)) - 0.5)))"},
        {"Box", "A rounded box.",
         {vec("size", Vec3{2, 2, 2}, 0.01f, 30.0f, "Half-size on each axis."),
          vec("center", Vec3{0, 0, 0}, -30.0f, 30.0f, "Middle."),
          num("round", 0.1f, 0.0f, 5.0f, "Corner radius.")},
         "(length(max(abs(p - $center) - $size + $round, vec3(0.0))) - $round)"},
        {"Torus", "A ring.",
         {num("major", 3.0f, 0.01f, 30.0f, "Radius of the ring."),
          num("minor", 0.6f, 0.01f, 10.0f, "Thickness.")},
         "(length(vec2(length(p.xz) - $major, p.y)) - $minor)"},
        {"Capsule", "A rounded bar between two points.",
         {vec("a", Vec3{0, -2, 0}, -30.0f, 30.0f, "One end."),
          vec("b", Vec3{0, 2, 0}, -30.0f, 30.0f, "Other end."),
          num("radius", 0.5f, 0.01f, 10.0f, "Thickness.")},
         "(length((p - $a) - ($b - $a) * clamp(dot(p - $a, $b - $a) / dot($b - $a, $b - $a), 0.0, 1.0)) - $radius)"},
        {"Ground", "An endless flat floor.",
         {num("height", -3.0f, -30.0f, 30.0f, "Height of the floor.")},
         "(p.y - $height)"},
    };
}

template <typename T>
const T* findByName(const std::vector<T>& list, std::string_view name) {
    for (const T& item : list) {
        if (item.name == name) return &item;
    }
    return nullptr;
}

nlohmann::json paramToJson(const ParamDesc& p) {
    nlohmann::json j;
    j["name"] = p.name;
    j["type"] = valueTypeName(p.type);
    if (p.type == ValueType::Float) {
        j["default"] = p.defaultValue.x;
    } else {
        j["default"] = {p.defaultValue.x, p.defaultValue.y, p.defaultValue.z};
    }
    j["min"] = p.min;
    j["max"] = p.max;
    j["description"] = p.description;
    return j;
}

template <typename T>
nlohmann::json paramsToJson(const T& item) {
    nlohmann::json out = nlohmann::json::array();
    for (const ParamDesc& p : item) out.push_back(paramToJson(p));
    return out;
}

}  // namespace

const char* stackName(Stack stack) {
    switch (stack) {
        case Stack::Spawn: return "spawn";
        case Stack::Move: return "move";
        case Stack::Look: return "look";
        default: return "surface";
    }
}

const char* valueTypeName(ValueType type) {
    switch (type) {
        case ValueType::Float: return "Float";
        case ValueType::Vec3: return "Vec3";
        default: return "Color";
    }
}

const std::vector<BlockDesc>& blocks() {
    static const std::vector<BlockDesc> list = makeBlocks();
    return list;
}

const std::vector<NodeDesc>& nodes() {
    static const std::vector<NodeDesc> list = makeNodes();
    return list;
}

const std::vector<ShapeDesc>& shapes() {
    static const std::vector<ShapeDesc> list = makeShapes();
    return list;
}

const BlockDesc* findBlock(std::string_view name) { return findByName(blocks(), name); }
const NodeDesc* findNode(std::string_view name) { return findByName(nodes(), name); }
const ShapeDesc* findShape(std::string_view name) { return findByName(shapes(), name); }

nlohmann::json catalogJson() {
    nlohmann::json out;
    out["blocks"] = nlohmann::json::array();
    for (const BlockDesc& b : blocks()) {
        nlohmann::json j;
        j["name"] = b.name;
        j["stack"] = stackName(b.stack);
        j["summary"] = b.summary;
        j["needsShape"] = b.needsShape;
        j["params"] = paramsToJson(b.params);
        out["blocks"].push_back(j);
    }
    out["nodes"] = nlohmann::json::array();
    for (const NodeDesc& n : nodes()) {
        nlohmann::json j;
        j["name"] = n.name;
        j["summary"] = n.summary;
        j["output"] = valueTypeName(n.output);
        j["inputs"] = paramsToJson(n.inputs);
        out["nodes"].push_back(j);
    }
    out["shapes"] = nlohmann::json::array();
    for (const ShapeDesc& s : shapes()) {
        nlohmann::json j;
        j["name"] = s.name;
        j["summary"] = s.summary;
        j["params"] = paramsToJson(s.params);
        out["shapes"].push_back(j);
    }
    return out;
}

}  // namespace iron::forge
