#pragma once

namespace iron {

// Canonical Vulkan (GLSL 450) "standard lit" shader — shadow PCF + normal map +
// Cook-Torrance GGX PBR + point lights + planar/cubemap reflection + fog + emissive.
// Single source of truth; games obtain handles via Renderer::createStandardLitShader().
// Consumes the shared LitUbo (set=0, binding=0) + bindings 1..8 (diffuse, normal,
// metallicRoughness, shadow, skyCubemap, reflection, AO, emissive) + 10/11/12
// (irradiance, prefiltered specular, BRDF LUT — M46b/c IBL); binding 9 = skinned bones.
// Keep in lockstep with VulkanRenderer's descriptor-set layout and engine/render/Pbr.h BRDF formulas.

inline const char* standardLitVertSource() {
    return R"GLSL(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;   // x=uvScale, y=roughness, z=reflectivity, w=shadowBias
    vec4 materialParams2;  // x=metallic, y=ao, z=normalScale, w=iblEnabled (M46b)
    vec4 baseColorFactor;  // M45c — xyz=albedo tint, w unused
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
    mat4 reflectionViewProj;  // M17 (unused in scene shader, here for layout parity)
    vec4 reflectionParams;    // M17 x=useReflectionPlane, yz=screenSize, w=0
    vec4 clipPlane;           // M17 — used only by reflection-pass shader
    vec4 probeBoxMin;   // M49
    vec4 probeBoxMax;   // M49
    vec4 probeCenter;   // M49 — w = probeActive
} u;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vLightSpacePos;

void main() {
    vec4 world = u.model * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(u.model) * aNormal;
    vTangent = mat3(u.model) * aTangent;
    vUV = aUV;
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * vec4(aPos, 1.0);
}
)GLSL";
}

inline const char* standardSkinnedLitVertSource() {
    return R"GLSL(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;
layout(location = 4) in uvec4 aJoints;
layout(location = 5) in vec4 aWeights;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;   // x=uvScale, y=roughness, z=reflectivity, w=shadowBias
    vec4 materialParams2;  // x=metallic, y=ao, z=normalScale, w=iblEnabled (M46b)
    vec4 baseColorFactor;  // M45c — xyz=albedo tint, w unused
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
    mat4 reflectionViewProj;  // M17 (unused in scene shader, here for layout parity)
    vec4 reflectionParams;    // M17 x=useReflectionPlane, yz=screenSize, w=0
    vec4 clipPlane;           // M17 — used only by reflection-pass shader
    vec4 probeBoxMin;   // M49
    vec4 probeBoxMax;   // M49
    vec4 probeCenter;   // M49 — w = probeActive
} u;

layout(set = 0, binding = 9) uniform BoneUbo {
    mat4 bones[128];
} bones;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vLightSpacePos;

void main() {
    // Weighted skinning: blend 4 bone matrices.
    mat4 skinMat = aWeights.x * bones.bones[aJoints.x]
                 + aWeights.y * bones.bones[aJoints.y]
                 + aWeights.z * bones.bones[aJoints.z]
                 + aWeights.w * bones.bones[aJoints.w];

    vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
    vec4 world      = u.model * skinnedPos;
    vWorldPos = world.xyz;
    vNormal   = mat3(u.model) * (mat3(skinMat) * aNormal);
    vTangent  = mat3(u.model) * (mat3(skinMat) * aTangent);
    vUV       = aUV;
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * skinnedPos;
}
)GLSL";
}

inline const char* standardLitFragSource() {
    return R"GLSL(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vTangent;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec4 vLightSpacePos;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;   // x=uvScale, y=roughness, z=reflectivity, w=shadowBias
    vec4 materialParams2;  // x=metallic, y=ao, z=normalScale, w=iblEnabled (M46b)
    vec4 baseColorFactor;  // M45c — xyz=albedo tint, w unused
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
    mat4 reflectionViewProj;  // M17 (unused in scene shader, here for layout parity)
    vec4 reflectionParams;    // M17 x=useReflectionPlane, yz=screenSize, w=0
    vec4 clipPlane;           // M17 — used only by reflection-pass shader
    vec4 probeBoxMin;   // M49
    vec4 probeBoxMax;   // M49
    vec4 probeCenter;   // M49 — w = probeActive
} u;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uMetallicRoughnessMap;
layout(set = 0, binding = 4) uniform sampler2D uShadowMap;
layout(set = 0, binding = 5) uniform samplerCube uSkyCubemap;
layout(set = 0, binding = 6) uniform sampler2D uReflection;
layout(set = 0, binding = 7) uniform sampler2D uAoMap;
layout(set = 0, binding = 8) uniform sampler2D uEmissiveMap;
layout(set = 0, binding = 10) uniform samplerCube uIrradianceCube;  // M46b diffuse IBL
layout(set = 0, binding = 11) uniform samplerCube uPrefiltered;   // M46c specular IBL
layout(set = 0, binding = 12) uniform sampler2D   uBrdfLut;       // M46c split-sum LUT

float shadowFactor(vec4 lightSpacePos, float bias) {
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (proj.z > 1.0) return 1.0;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored = texture(uShadowMap, uv + vec2(x, y) * texel).r;
            sum += (proj.z - bias > stored) ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}

// Cook-Torrance GGX PBR helpers — lockstep with engine/render/Pbr.h.
const float PI = 3.14159265359;
vec3 fresnelSchlick(float cosT, vec3 F0) { return F0 + (1.0 - F0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0); }
float distributionGGX(float nDotH, float rough) {
    float a = rough*rough; float a2 = a*a;
    float d = (nDotH*nDotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}
float geometrySmith(float nDotV, float nDotL, float rough) {
    float r = rough + 1.0; float k = (r*r) / 8.0;
    float gv = nDotV / (nDotV * (1.0 - k) + k);
    float gl = nDotL / (nDotL * (1.0 - k) + k);
    return gv * gl;
}
vec3 pbrContrib(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float rough, vec3 F0, vec3 radiance) {
    vec3 H = normalize(V + L);
    float nDotV = max(dot(N, V), 1e-4);
    float nDotL = max(dot(N, L), 0.0);
    float nDotH = max(dot(N, H), 0.0);
    float D = distributionGGX(nDotH, rough);
    float G = geometrySmith(nDotV, nDotL, rough);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 spec = (D * G) * F / (4.0 * nDotV * nDotL + 1e-4);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
    return (kd * albedo / PI + spec) * radiance * nDotL;
}
vec3 fresnelSchlickRoughness(float cosT, vec3 F0, float rough) {
    return F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}

void main() {
    float uvScale = u.materialParams.x;
    float bias    = u.materialParams.w;
    vec2 uv = vUV * uvScale;

    vec3 N = normalize(vNormal);
    vec3 T = normalize(vTangent);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    float normalScale = u.materialParams2.z;
    vec3 tangentNormal = texture(uNormalMap, uv).rgb * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;   // glTF normalTexture.scale (0=flat, >1=exaggerated)
    vec3 perturbedN = normalize(TBN * tangentNormal);

    // PBR material parameters.
    vec3  albedo    = texture(uDiffuse, uv).rgb * u.baseColorFactor.xyz;
    float roughness = clamp(u.materialParams.y * texture(uMetallicRoughnessMap, uv).g, 0.04, 1.0);
    float metallic  = clamp(u.materialParams2.x * texture(uMetallicRoughnessMap, uv).b, 0.0, 1.0);
    float ao        = u.materialParams2.y * texture(uAoMap, uv).r;
    vec3  F0        = mix(vec3(0.04), albedo, metallic);
    vec3  V         = normalize(u.cameraPos.xyz - vWorldPos);
    vec3  N_        = perturbedN;

    float shadow = shadowFactor(vLightSpacePos, bias);

    // Directional light (sun).
    vec3 Lo = pbrContrib(N_, V, -normalize(u.sunDir.xyz), albedo, metallic, roughness, F0,
                         u.sunColor.xyz) * shadow;

    // Point lights (M15).
    int plCount = int(u.lightCounts.x);
    for (int i = 0; i < plCount; ++i) {
        vec3 toLight = u.pointPositions[i].xyz - vWorldPos;
        float dist  = length(toLight);
        float range = u.pointColors[i].w;
        if (dist < 0.0001 || dist >= range) continue;
        vec3 Lp = toLight / dist;
        float falloff   = 1.0 - smoothstep(0.0, range, dist);
        float intensity = u.pointPositions[i].w;
        vec3 radiance = u.pointColors[i].xyz * intensity * falloff;
        Lo += pbrContrib(N_, V, Lp, albedo, metallic, roughness, F0, radiance);
    }

    // M46b — diffuse IBL: when an irradiance map is bound (iblEnabled), use the
    // environment irradiance; otherwise the legacy flat ambient.
    vec3 ambient;
    if (u.materialParams2.w > 0.5) {
        // M46c — split-sum IBL: diffuse irradiance + prefiltered specular.
        float nDotV = max(dot(N_, V), 0.0);
        vec3  F     = fresnelSchlickRoughness(nDotV, F0, roughness);
        vec3  kD    = (vec3(1.0) - F) * (1.0 - metallic);
        vec3  diffuseIBL  = texture(uIrradianceCube, N_).rgb * albedo;
        vec3  R           = reflect(-V, N_);
        if (u.probeCenter.w > 0.5) {  // M49 — box-projected parallax correction
            // M49 — box-projected parallax correction toward local geometry.
            // Mirror the CPU math in ReflectionProbe.h: pick the slab exit plane
            // in the ray direction per axis; a zero-direction axis gets t=+big so
            // min() excludes it (a tiny-epsilon reciprocal would flip the sign).
            vec3 farPlane = mix(u.probeBoxMin.xyz, u.probeBoxMax.xyz, step(0.0, R));
            vec3 t3;
            t3.x = (R.x != 0.0) ? (farPlane.x - vWorldPos.x) / R.x : 1e30;
            t3.y = (R.y != 0.0) ? (farPlane.y - vWorldPos.y) / R.y : 1e30;
            t3.z = (R.z != 0.0) ? (farPlane.z - vWorldPos.z) / R.z : 1e30;
            float t  = min(min(t3.x, t3.y), t3.z);
            vec3  hit = vWorldPos + R * t;
            R = hit - u.probeCenter.xyz;
        }
        float maxMip      = float(textureQueryLevels(uPrefiltered) - 1);
        vec3  prefiltered = textureLod(uPrefiltered, R, roughness * maxMip).rgb;
        vec2  brdf        = texture(uBrdfLut, vec2(nDotV, roughness)).rg;
        vec3  specularIBL = prefiltered * (F * brdf.x + brdf.y);
        ambient = (kD * diffuseIBL + specularIBL) * ao;
    } else {
        ambient = u.ambient.xyz * albedo * ao;
    }
    vec3 emissive = u.emissive.xyz * texture(uEmissiveMap, uv).rgb;
    vec3 color = ambient + Lo + emissive;

    // M17 — planar reflection (preferred when active) with M16 cubemap fallback.
    float reflectivity = u.materialParams.z;
    if (u.reflectionParams.x > 0.5) {
        vec2 ndc = gl_FragCoord.xy / u.reflectionParams.yz;
        vec3 reflectColor = texture(uReflection, ndc).rgb;
        color = mix(color, reflectColor, reflectivity);
    } else if (reflectivity > 0.0 && u.materialParams2.w < 0.5) {
        // M16 crude sky-cube reflection — superseded by split-sum IBL specular when a
        // skybox/IBL is present (iblEnabled); only runs as the legacy fallback (IBL off).
        vec3 viewDir = normalize(vWorldPos - u.cameraPos.xyz);
        vec3 reflectDir = reflect(viewDir, perturbedN);
        vec3 reflectColor = texture(uSkyCubemap, reflectDir).rgb;
        color = mix(color, reflectColor, reflectivity);
    }

    // Fog (M15).
    float distFromCamera = length(u.cameraPos.xyz - vWorldPos);
    float fogFactor = 1.0 - exp(-u.fogColor.w * distFromCamera);
    vec3 finalColor = mix(color, u.fogColor.xyz, clamp(fogFactor, 0.0, 1.0));
    outColor = vec4(finalColor, 1.0);
}
)GLSL";
}

}  // namespace iron
