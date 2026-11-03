#pragma once

// GLSL shared by the GI probe passes and the lit shaders: the volume's
// parameters, where a probe is and where its texels lie in the atlases, the
// octahedral mapping of directions to texels, and the visibility-weighted
// lookup a surface uses to take its light from the eight probes round it.
//
// The including shader declares the parameter block as `gi` (GiParamsGlsl's
// members) and the probes' offsets as `giOffsets` (a vec4 per probe: xyz how far
// relocation moved it off its grid point, out of walls) before this text.

namespace iron {

// The members of the GI parameter block (std140), for the including shader to wrap.
inline const char* giParamsMembersGlsl() {
    return R"GLSL(
    vec4  origin;     // xyz the first probe; w = 1 when the volume is live
    vec4  spacing;    // xyz between probes; w = view bias scale
    ivec4 counts;     // xyz probes per axis; w = rays per probe
    vec4  atlas;      // irradiance atlas size (xy), distance atlas size (zw), in texels
    mat4  rotation;   // This frame's random turn of the ray directions
    vec4  sun;        // xyz toward the sun; w = hysteresis
    vec4  sunColor;   // rgb; w = light count
    vec4  misc;       // x = max ray distance; y = view (GiView); z = energy; w = frames since reset
    vec4  boundsMin;  // The volume's box: xyz min
    vec4  boundsMax;  // xyz max
)GLSL";
}

inline const char* giCommonGlsl() {
    return R"GLSL(
const int GI_IRRADIANCE_TEXELS = 6;   // Per probe, inside a one-texel border.
const int GI_DISTANCE_TEXELS = 14;

vec2 giSignNotZero(vec2 v) { return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 giOctEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 p = n.xy;
    if (n.z < 0.0) p = (1.0 - abs(p.yx)) * giSignNotZero(p);
    return p;
}
vec3 giOctDecode(vec2 p) {
    vec3 n = vec3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * giSignNotZero(n.xy);
    return normalize(n);
}
ivec3 giProbeCoord(int index) {
    return ivec3(index % gi.counts.x, (index / gi.counts.x) % gi.counts.y, index / (gi.counts.x * gi.counts.y));
}
int giProbeIndex(ivec3 c) { return c.x + gi.counts.x * (c.y + gi.counts.y * c.z); }
vec3 giProbeGrid(ivec3 c) { return gi.origin.xyz + vec3(c) * gi.spacing.xyz; }
vec3 giProbePos(ivec3 c) { return giProbeGrid(c) + giOffsets[giProbeIndex(c)].xyz; }
// A probe's tile in the atlases: x and y along a row, z down the rows.
ivec2 giTile(ivec3 c) { return ivec2(c.x + c.y * gi.counts.x, c.z); }
vec2 giAtlasUv(ivec3 c, vec3 dir, int texels, vec2 atlasSize) {
    vec2 o = giOctEncode(dir) * 0.5 + 0.5;
    vec2 px = vec2(giTile(c)) * float(texels + 2) + 1.0 + o * float(texels);
    return px / atlasSize;
}

// How much the volume lights `p`: fully anywhere within its probes, fading to
// nothing over a probe spacing beyond them.
float giVolumeWeight(vec3 p) {
    if (gi.origin.w < 0.5) return 0.0;
    vec3 outside = max(max(gi.boundsMin.xyz - p, p - gi.boundsMax.xyz), vec3(0.0)) / gi.spacing.xyz;
    return clamp(1.0 - max(outside.x, max(outside.y, outside.z)), 0.0, 1.0);
}

// The diffuse light arriving at `p` with normal `n`, seen from direction `v`
// (toward the eye): divided by pi, so albedo times it is the light reflected.
vec3 giIrradiance(sampler2D irradianceAtlas, sampler2D distanceAtlas, vec3 p, vec3 n, vec3 v) {
    float minSpacing = min(gi.spacing.x, min(gi.spacing.y, gi.spacing.z));
    // Pushed off the surface a little, toward the eye, so it doesn't see itself.
    vec3 biased = p + (n * 0.6 + v * 0.4) * (0.75 * minSpacing) * gi.spacing.w;
    ivec3 base = clamp(ivec3(floor((biased - gi.origin.xyz) / gi.spacing.xyz)), ivec3(0), gi.counts.xyz - 2);
    vec3 alpha = clamp((biased - giProbeGrid(base)) / gi.spacing.xyz, 0.0, 1.0);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = 0; i < 8; ++i) {
        ivec3 off = ivec3(i, i >> 1, i >> 2) & ivec3(1);
        ivec3 c = base + off;
        vec3 probe = giProbePos(c);
        vec3 tri = mix(1.0 - alpha, alpha, vec3(off));
        // Probes behind the surface count for less.
        vec3 toProbe = normalize(probe - p);
        float wrap = (dot(toProbe, n) + 1.0) * 0.5;
        float weight = wrap * wrap + 0.2;
        // And probes that can't see it (Chebyshev, on its distances that way), much less.
        vec3 fromProbe = biased - probe;
        float r = length(fromProbe);
        vec2 moments = textureLod(distanceAtlas, giAtlasUv(c, fromProbe / max(r, 1e-4), GI_DISTANCE_TEXELS, gi.atlas.zw), 0.0).rg;
        float visible = 1.0;
        if (r > moments.x) {
            float variance = abs(moments.y - moments.x * moments.x);
            float d = r - moments.x;
            visible = variance / (variance + d * d);
            visible = visible * visible * visible;
        }
        weight *= visible;
        weight = max(weight, 1e-6);
        // Crush the tiny weights, so a sliver of a hidden probe doesn't tint it;
        // then the trilinear weight, after, so the blend between probes stays linear.
        const float crush = 0.2;
        if (weight < crush) weight *= weight * weight / (crush * crush);
        weight *= tri.x * tri.y * tri.z;
        // Stored as irradiance^(1/5) (see the blend): back to light.
        vec3 encoded = textureLod(irradianceAtlas, giAtlasUv(c, n, GI_IRRADIANCE_TEXELS, gi.atlas.xy), 0.0).rgb;
        vec3 e2 = encoded * encoded;
        vec3 irradiance = e2 * e2 * encoded;
        sum += irradiance * weight;
        total += weight;
    }
    return sum / max(total, 1e-6);
}
)GLSL";
}

}  // namespace iron
