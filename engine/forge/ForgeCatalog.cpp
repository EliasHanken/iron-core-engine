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

    list.push_back({"Follow", Stack::Move, "Chase a moving point, each particle at its own pace.",
        {vec("target", Vec3{0, 0, 0}, -100.0f, 100.0f, "The point to chase. Wire an InputVector here."),
         num("fast", 6.0f, 0.0f, 30.0f, "How quickly the keenest particles catch up."),
         num("slow", 0.8f, 0.0f, 30.0f, "How quickly the laziest catch up. Lower makes a longer tail."),
         num("grip", 8.0f, 0.1f, 30.0f, "How sharply they turn: lower swings wide."),
         num("spread", 1.0f, 0.0f, 20.0f, "Size of the cloud around the point, in metres.")},
        R"({
    uint own = uint(seedValue * 4000000.0);
    vec3 home = $target + randomDirection(own) * $spread * pow(rnd(own + 3u), 0.5);
    float pace = mix($slow, $fast, seedValue * seedValue);
    v = mix(v, (home - x) * pace, 1.0 - exp(-dt * $grip));
})"});
    list.push_back({"Blast", Stack::Move, "Throw particles away from a point.",
        {vec("point", Vec3{0, 0, 0}, -100.0f, 100.0f, "Middle of the blast."),
         num("amount", 0.0f, 0.0f, 1.0f, "0 is off, 1 is full force. Wire an Input here."),
         num("strength", 120.0f, 0.0f, 600.0f, "Push in metres per second squared."),
         num("radius", 12.0f, 0.1f, 60.0f, "How far it reaches.")},
        R"({
    vec3 away = x - $point;
    float d = length(away) + 1e-4;
    v += away / d * $amount * $strength * max(1.0 - d / $radius, 0.0) * dt;
})"});

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
    list.push_back({"FadeNearCamera", Stack::Look, "Fade out close to the camera, so that nothing blinds it.",
        {num("start", 3.0f, 0.0f, 50.0f, "Fully visible beyond this distance."),
         num("end", 0.5f, 0.0f, 50.0f, "Gone within this distance.")},
        R"(alpha *= smoothstep($end, $start, length(x - frame.cameraPos.xyz));)"});
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
    list.push_back({"Plating", Stack::Surface, "Bolted steel plates, with dark seams and rivets.",
        {num("size", 2.5f, 0.1f, 20.0f, "Width of a plate in metres."),
         num("seam", 0.02f, 0.0f, 0.3f, "Width of the gap between plates, in metres."),
         num("vary", 0.5f, 0.0f, 1.0f, "How much plates differ from each other.")},
        R"({
    vec2 flat2 = abs(N.x) > 0.5 ? worldPos.zy : (abs(N.y) > 0.5 ? worldPos.xz : worldPos.xy);
    vec2 plate = flat2 / $size;
    vec2 id = floor(plate);
    vec2 inner = (fract(plate) - 0.5) * $size;
    vec2 toEdge = 0.5 * $size - abs(inner);
    float edge = min(toEdge.x, toEdge.y);
    float shade = hash31(vec3(id, 7.0));
    albedo *= 1.0 + (shade - 0.5) * $vary;
    roughness = clamp(roughness + (shade - 0.5) * 0.3 * $vary, 0.04, 1.0);
    vec2 rivetCell = fract(flat2 / 0.4) - 0.5;
    float rivet = (1.0 - smoothstep(0.1, 0.17, length(rivetCell))) * step(edge, 0.1);
    albedo *= 1.0 + rivet * 0.5;
    albedo *= smoothstep(0.0, $seam + 1e-3, edge - $seam);
})"});
    list.push_back({"Rust", Stack::Surface, "Patches of rust that run downward.",
        {color("color", Vec3{0.20f, 0.075f, 0.03f}, "Colour of the rust."),
         num("amount", 0.5f, 0.0f, 1.0f, "How much of the surface it covers."),
         num("scale", 0.4f, 0.02f, 8.0f, "Size of the patches: higher is finer.")},
        R"({
    float patches = fbm(worldPos * vec3(1.0, 0.3, 1.0) * $scale + vec3(3.1), 5);
    float fine = fbm(worldPos * $scale * 7.0, 3);
    float rust = smoothstep(0.62 - $amount * 0.3, 0.7 - $amount * 0.3, patches + (fine - 0.5) * 0.25);
    albedo = mix(albedo, $color * (0.6 + fine * 0.8), rust);
    metallic = mix(metallic, 0.0, rust);
    roughness = mix(roughness, 0.92, rust);
})"});
    list.push_back({"Grime", Stack::Surface, "Dirt that gathers low down and streaks down walls.",
        {num("amount", 0.6f, 0.0f, 1.0f, "How dark it gets."),
         num("height", -3.0f, -100.0f, 100.0f, "Where it starts, in metres. Lower down is dirtier.")},
        R"({
    float low = 1.0 - smoothstep($height - 3.0, $height + 3.0, worldPos.y);
    float streaks = fbm(worldPos * vec3(1.6, 0.1, 1.6), 4);
    float dirt = clamp(low * 0.7 + smoothstep(0.5, 0.75, streaks) * 0.7, 0.0, 1.0);
    albedo *= 1.0 - dirt * $amount;
    roughness = mix(roughness, 0.85, dirt * 0.6);
})"});
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
        {"Input", "A number the game sets while running.",
         {num("value", 0.0f, -1000.0f, 1000.0f, "Value until the game sets one.")},
         ValueType::Float, "", true},
        {"InputVector", "A position or colour the game sets while running.",
         {vec("value", Vec3{0, 0, 0}, -1000.0f, 1000.0f, "Value until the game sets one.")},
         ValueType::Vec3, "", true},
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
        {"Room", "The inside of a box: things stay in.",
         {vec("size", Vec3{20, 6, 20}, 0.01f, 200.0f, "Half-size on each axis."),
          vec("center", Vec3{0, 0, 0}, -100.0f, 100.0f, "Middle.")},
         "forgeRoom(p, $center, $size)"},
        {"Pillars", "A grid of square pillars.",
         {num("spacing", 10.0f, 0.1f, 100.0f, "Distance between pillars."),
          num("width", 0.8f, 0.01f, 50.0f, "Half-width of a pillar."),
          num("from", -1000.0f, -1000.0f, 1000.0f, "No pillars below this z.")},
         "forgePillars(p, $spacing, $width, $from)"},
        {"JoinedRooms", "The inside of two rooms and a round chamber, as one space.",
         {vec("sizeA", Vec3{20, 6, 20}, 0.01f, 200.0f, "Half-size of the first room."),
          vec("centerA", Vec3{0, 0, 0}, -200.0f, 200.0f, "Middle of the first room."),
          vec("sizeB", Vec3{1, 3, 5}, 0.01f, 200.0f, "Half-size of the second room."),
          vec("centerB", Vec3{0, 0, -24}, -200.0f, 200.0f, "Middle of the second room."),
          vec("domeCenter", Vec3{0, 0, -60}, -200.0f, 200.0f, "Middle of the chamber."),
          num("domeRadius", 34.0f, 0.01f, 200.0f, "Radius of the chamber.")},
         "max(max(forgeRoom(p, $centerA, $sizeA), forgeRoom(p, $centerB, $sizeB)), "
         "$domeRadius - length(p - $domeCenter))"},
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
