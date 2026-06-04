#pragma once

#include "math/Mat4.h"
#include "math/Vec.h"
#include "render/Fog.h"
#include "render/Handles.h"
#include "render/HudBatch.h"
#include "render/Light.h"
#include "render/Material.h"
#include "render/PostEffect.h"
#include "render/ReflectionPlane.h"
#include "render/ReflectionProbe.h"
#include "render/StandardLitShader.h"
#include "scene/Mesh.h"
#include "asset/Skeleton.h"
#include "scene/SkinnedMesh.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace iron {

// Maximum point lights uploaded to the lit shader per frame. The lit
// fragment shader declares a uniform array of this size. Extras passed
// to beginFrame are silently dropped (the renderer logs a warning each
// overflow frame).
constexpr int kMaxPointLights = 16;

// One thing to draw: a mesh, a shader, a model matrix, and a material.
// All per-surface properties (texture, emissive glow, reflectivity, UV tiling)
// live on the embedded Material so they can be composed and reused independently
// of the draw mechanics (mesh, shader, transform).
struct DrawCall {
    MeshHandle mesh = kInvalidHandle;
    ShaderHandle shader = kInvalidHandle;
    Mat4 model = Mat4::identity();
    Material material{};
    uint8_t effectId = 0;   // 0 = no post-process effect; else an EffectTable id (M36)
};

// M23 — maximum bone matrices uploaded per skinned draw call. The
// skinned vertex shader's `uBones` array is sized to this constant; any
// excess bones supplied via SkinnedDrawCall::boneMatrices are silently
// dropped (the joint indices in the mesh wouldn't reference them anyway
// for well-formed glTF imports).
inline constexpr std::size_t kMaxBonesPerSkinnedMesh = 128;

// M23 — one skinned thing to draw. Mirrors DrawCall (mesh+shader+model+material)
// but adds a per-instance array of bone matrices (one mat4 per skeleton
// joint, in skeleton order). The skinning shader multiplies vertex
// positions/normals by the four-weighted blend of these matrices.
struct SkinnedDrawCall {
    SkinnedMeshHandle skinnedMesh = kInvalidSkinnedMesh;
    ShaderHandle      shader      = kInvalidHandle;
    Mat4              model       = Mat4::identity();
    Material          material{};
    std::span<const Mat4> boneMatrices;  // size <= kMaxBonesPerSkinnedMesh
};

// Render Hardware Interface: a graphics-API-agnostic renderer. Game code talks
// only to this interface; concrete backends (OpenGLRenderer today, others
// later) implement it. Keeping this surface small is deliberate — it is the
// contract every future backend must honour.
//
// Resource lifetime: resources created here live until the Renderer is
// destroyed (application-scoped). There is intentionally no per-resource
// destroy API yet — explicit destruction lands when a game needs to stream
// resources, and will be added as a deliberate interface change then.
class Renderer {
public:
    virtual ~Renderer() = default;

    // --- resource creation ---
    virtual MeshHandle createMesh(const MeshData& data) = 0;
    // Replace the geometry of an existing mesh (for meshes rebuilt per frame).
    virtual void updateMesh(MeshHandle mesh, const MeshData& data) = 0;
    // RGBA8 pixels, `width * height * 4` bytes, row-major from top-left.
    // Pass srgb=false for non-color data (normal maps, metallic-roughness, AO)
    // so the hardware samples linear values instead of applying sRGB decode.
    virtual TextureHandle createTexture(int width, int height,
                                        const unsigned char* rgba,
                                        bool srgb = true) = 0;
    // Loads an image file (PNG/JPG) into a texture. Returns kInvalidHandle on
    // failure. Pass srgb=false for non-color data (normal/MR/AO maps).
    virtual TextureHandle loadTexture(const std::string& path,
                                      bool srgb = true) = 0;
    // A built-in 1x1 opaque-white texture, handy for solid-colour quads.
    virtual TextureHandle whiteTexture() const = 0;
    // A built-in 1x1 "flat normal" texture (RGB 128,128,255 = +Z in tangent
    // space). Bound to the normal-map sampler when a draw's material doesn't
    // set normalMap, so the shader's TBN sample returns the geometric normal
    // unchanged.
    virtual TextureHandle flatNormalTexture() const = 0;
    // A built-in 1x1 "no specular" texture (RGB 0,0,0). Legacy from the
    // Blinn-Phong era; the PBR path (M45b) defaults missing metallic-roughness
    // and AO maps to whiteTexture() instead. Retained for the frozen OpenGL
    // backend's interface.
    virtual TextureHandle noSpecularTexture() const = 0;
    // Compiles + links a shader from GLSL source. Returns kInvalidHandle on
    // failure.
    virtual ShaderHandle createShader(const std::string& vertexSrc,
                                      const std::string& fragmentSrc) = 0;

    // --- M23: Skinned mesh + draw API ---
    // Creates a GPU-resident skinned mesh from CPU-side SkinnedMeshData.
    // Returns kInvalidSkinnedMesh on failure or empty input. Vulkan-only;
    // the OpenGL backend stubs this and warns once.
    virtual SkinnedMeshHandle createSkinnedMesh(const SkinnedMeshData& data) = 0;
    // Creates a shader for the skinned pipeline. The vertex shader must
    // accept the SkinnedVertex layout (position, normal, uv, tangent,
    // joints[uvec4], weights[vec4]) and sample bone matrices from
    // descriptor set binding 7 (a uniform buffer of mat4[kMaxBonesPerSkinnedMesh]).
    virtual ShaderHandle createSkinnedShader(const std::string& vertexSrc,
                                              const std::string& fragmentSrc) = 0;

    // Convenience: create the engine's canonical standard lit shader (M45a).
    // Concrete — delegates to the backend's createShader with engine-owned GLSL.
    // Vulkan-only sources; the frozen OpenGL backend's games keep inline GLSL 330.
    ShaderHandle createStandardLitShader() {
        return createShader(standardLitVertSource(), standardLitFragSource());
    }
    ShaderHandle createStandardSkinnedLitShader() {
        return createSkinnedShader(standardSkinnedLitVertSource(), standardLitFragSource());
    }

    // M28 — hot-reload: replace the GLSL behind an existing shader handle.
    // Recompiles to SPIR-V, recreates the shader modules (keeping the same
    // descriptor + pipeline layout — the shader interface must be unchanged),
    // and invalidates any pipeline cached against the shader so the next draw
    // rebuilds it. Returns true on success. On compile failure the previous
    // shader stays bound and false is returned (a typo in a live edit keeps
    // the last-good shader instead of crashing). Vulkan-only; OpenGL warns
    // once and returns false.
    virtual bool reloadShader(ShaderHandle handle,
                              const std::string& vertexSrc,
                              const std::string& fragmentSrc) = 0;

    // Records one skinned draw call for this frame. boneMatrices contains
    // the per-joint transforms in skeleton order; missing slots default to
    // identity.
    virtual void submitSkinnedDraw(const SkinnedDrawCall& call) = 0;

    // Creates a cubemap texture from six RGBA face arrays. Each face is
    // `width * height * 4` bytes. Face order: +X, -X, +Y, -Y, +Z, -Z.
    // Returns kInvalidHandle if any face is null or dimensions invalid.
    virtual CubemapHandle createCubemap(
        int width, int height,
        const std::array<const unsigned char*, 6>& faces) = 0;

    // Registers a cubemap as the skybox for subsequent frames. Pass
    // kInvalidHandle to disable the skybox.
    virtual void setSkybox(CubemapHandle sky) = 0;

    // Loads an equirectangular Radiance .hdr from disk, bakes it into an
    // RGBA16F cubemap, and returns its handle (usable with setSkybox). The
    // path is resolved relative to the working directory. `faceSize` is the
    // per-face resolution of the baked cube. Returns kInvalidHandle on
    // failure (e.g. file not found, or on backends without IBL support).
    virtual CubemapHandle loadHdrSkybox(const std::string& hdrPath,
                                        int faceSize = 512) = 0;

    // M49 — set the active reflection probes for this scene. Each draw selects
    // the nearest probe whose box contains it; outside all probes, the global
    // skybox IBL is used. Pass an empty span to disable probes.
    virtual void setReflectionProbes(std::span<const GpuReflectionProbe> probes) = 0;

    // M49 — bake (capture + prefilter) every probe. On-demand; blocks. The
    // probe vector is updated in place with the baked prefiltered handles.
    virtual void bakeReflectionProbes(std::vector<GpuReflectionProbe>& probes) = 0;

    // --- per-frame ---
    // Begins a frame: records the clear colour, the directional sun, the
    // per-frame point lights (capped to kMaxPointLights), the fog, and the
    // camera (view + projection). Submitted draw calls are buffered until
    // endFrame.
    virtual void beginFrame(Vec3 clearColor, const DirectionalLight& light,
                            std::span<const PointLight> pointLights,
                            const Fog& fog,
                            const Mat4& view, const Mat4& projection) = 0;
    // Records one draw call for this frame. Each DrawCall supplies its model
    // matrix; the camera comes from beginFrame.
    virtual void submit(const DrawCall& call) = 0;
    // Renders every buffered draw call for the frame.
    virtual void endFrame() = 0;

    // Sets the world-space sphere (centre + radius) the directional light's
    // shadow map must cover. A game calls this once with bounds enclosing its
    // scene.
    virtual void setShadowBounds(Vec3 center, float radius) = 0;

    // Sets the world-space reflection plane. The renderer will run an extra
    // planar reflection pass per frame using a camera mirrored across this
    // plane; any DrawCall with material.useReflectionPlane=true samples the
    // resulting texture in screen space. `normal` must be unit length.
    virtual void setReflectionPlane(Vec3 normal, float d) = 0;

    // Disables the planar reflection pass. Reflective DrawCalls with
    // material.useReflectionPlane=true will then sample the cubemap as a fallback.
    virtual void disableReflectionPlane() = 0;

    // Configure the post-process effect style for an id referenced by
    // DrawCall::effectId. Vulkan-only; the base implementation ignores it
    // (the OpenGL backend has no post-process chain). (M36)
    virtual void setEffectStyle(uint8_t effectId, const EffectStyle& style) {
        (void)effectId; (void)style;
    }

    // --- debug drawing ---
    // Queue a coloured 3D line segment for the current frame.
    virtual void drawLine(Vec3 a, Vec3 b, Vec3 color) = 0;
    // Draw all queued debug lines (depth-tested) and clear the queue.
    virtual void flushDebugLines(const Mat4& view, const Mat4& projection) = 0;
    // Like drawLine, but drawn on top of geometry (depth test disabled) — for
    // editor gizmos / manipulators that must stay visible. Default forwards to
    // drawLine (depth-tested); the Vulkan backend overrides it with an overlay
    // pass. Flushed by flushDebugLines alongside the regular lines.
    virtual void drawLineOverlay(Vec3 a, Vec3 b, Vec3 color) { drawLine(a, b, color); }
    // Like drawLineOverlay, but rendered thicker (when the device supports
    // wideLines) — for gizmo handles that should read as chunky manipulators
    // while thin overlay lines (e.g. selection outlines) stay 1px. Default
    // forwards to drawLineOverlay.
    virtual void drawLineOverlayThick(Vec3 a, Vec3 b, Vec3 color) { drawLineOverlay(a, b, color); }
    // Queue a filled, alpha-blended, always-on-top triangle — for translucent
    // gizmo plane / center handles. Default no-op (only Vulkan implements it).
    virtual void drawTriOverlay(Vec3 a, Vec3 b, Vec3 c, Vec3 color) {
        (void)a; (void)b; (void)c; (void)color;
    }

    // --- HUD ---
    // Draw a screen-space HUD batch on top of the scene, sized to the given
    // framebuffer dimensions. Call BEFORE endFrame — under Vulkan the HUD
    // records into the active scene render pass, so the per-frame command
    // buffer must still be open. Under OpenGL the order is flexible, but
    // calling before endFrame works for both backends and is the supported
    // contract going forward.
    virtual void drawHud(const HudBatch& batch, int framebufferWidth,
                         int framebufferHeight) = 0;

    // Call when the framebuffer is resized.
    virtual void setViewport(int width, int height) = 0;
};

} // namespace iron
