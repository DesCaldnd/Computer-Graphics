#pragma once

// C++ mirrors of the GPU structures declared in engine/shaders/render/common/*.glsl. Every struct uses the scalar
// block layout (tight C++ packing): keep both sides in sync and extend the static_asserts when adding fields.

#include <oxwald/core/math.hpp>
#include <oxwald/core/types.hpp>

namespace ox::render {

inline constexpr u32 kMaxCascades = 4;
inline constexpr u32 kInvalidIndex = ~0u;

// GpuInstance::flags
enum GpuInstanceFlags : u32 {
    kInstanceCastShadows = 1u << 0,
    kInstanceReceiveShadows = 1u << 1,
    kInstanceSkinned = 1u << 2,
    kInstanceMoved = 1u << 3, // world != previous this frame (shadow cache invalidation, velocity)
    kInstanceSelected = 1u << 4,
    kInstanceVisible = 1u << 5,
};

// One drawable = one (entity, submesh) pair. Persistent buffer, updated incrementally.
struct GpuInstance {
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};
    glm::vec4 boundingSphere{0.0f}; // world space xyz + radius
    u32 meshIndex = 0;              // GpuMeshInfo (submesh) index
    u32 materialIndex = 0;
    u32 flags = 0;
    u32 entityId = 0;              // encodeEntityId(entt) for picking; 0 = none
    u32 paletteOffset = kInvalidIndex;     // into GpuSceneHeader::palettes (mat4), skinned only
    u32 prevPaletteOffset = kInvalidIndex;
    u32 userData = 0;
    u32 pad = 0;
};
static_assert(sizeof(GpuInstance) == 176);

// Draw information of one submesh (LOD 0). Geometry lives in shared arenas (positions / attributes / indices).
struct GpuMeshInfo {
    u32 firstIndex = 0;
    u32 indexCount = 0;
    i32 vertexOffset = 0; // base vertex of the mesh in the arenas (indices are mesh-relative)
    u32 vertexCount = 0;
    glm::vec4 boundingSphere{0.0f}; // mesh space
    glm::vec3 aabbMin{0.0f};
    u32 meshletOffset = 0; // into the meshlet arena (for GPU-driven culling)
    glm::vec3 aabbMax{0.0f};
    u32 meshletCount = 0;
    u32 skinOffset = kInvalidIndex; // SkinVertex arena offset of vertex 0 (skinned meshes)
    u32 lodCount = 1;
    u32 pad0 = 0, pad1 = 0;
};
static_assert(sizeof(GpuMeshInfo) == 80);

enum GpuMaterialFlags : u32 {
    kMaterialBlendMask = 0x7u, // assets::BlendMode (Opaque, AlphaTest, Transparent, Refractive)
    kMaterialDoubleSided = 1u << 3,
    kMaterialUnlit = 1u << 4,
};

struct GpuMaterial {
    glm::vec4 baseColor{1.0f};
    glm::vec3 emissive{0.0f}; // linear, already multiplied by emissiveStrength (in nits-ish scene units)
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    f32 normalStrength = 1.0f;
    f32 occlusionStrength = 1.0f;
    f32 alphaCutoff = 0.5f;
    u32 albedoTexture = kInvalidIndex; // bindless sampled indices (kInvalidIndex = none)
    u32 normalTexture = kInvalidIndex;
    u32 ormTexture = kInvalidIndex;
    u32 emissiveTexture = kInvalidIndex;
    u32 flags = 0;
    u32 sampler = 4; // bindless sampler index (default anisotropic repeat)
    f32 ior = 1.5f;
    f32 transmission = 0.0f;
    glm::vec3 absorptionColor{1.0f};
    f32 absorptionDistance = 0.0f;
    f32 thickness = 0.0f;
    f32 clearcoat = 0.0f;
    f32 clearcoatRoughness = 0.0f;
    f32 subsurface = 0.0f;
    glm::vec2 uvTiling{1.0f};
    glm::vec2 uvOffset{0.0f};
};
static_assert(sizeof(GpuMaterial) == 128);

enum class GpuLightType : u32 { Directional = 0, Point = 1, Spot = 2 };

struct GpuLight {
    glm::vec3 position{0.0f};
    f32 range = 10.0f;
    glm::vec3 color{1.0f}; // linear colour × intensity (candela for point/spot, lux for directional)
    u32 type = 0;          // GpuLightType
    glm::vec3 direction{0.0f, -1.0f, 0.0f}; // direction the light travels (spot/directional)
    f32 sourceRadius = 0.0f;
    f32 spotScale = 0.0f;  // 1 / max(cos(inner) - cos(outer), eps)
    f32 spotOffset = 1.0f; // -cos(outer) * spotScale
    i32 shadowIndex = -1;  // GpuShadow index, -1 = unshadowed
    u32 entityId = 0;
};
static_assert(sizeof(GpuLight) == 64);

enum class GpuShadowKind : u32 { Spot = 0, Point = 1 };

struct GpuShadow {
    glm::mat4 viewProj{1.0f};   // spot: light view-projection (Vulkan clip, reversed-Z)
    glm::vec4 atlasRect{0.0f};  // spot: uv offset (xy) + scale (zw) in the atlas; point: x = tan(fov/2), y = face size
    f32 bias = 0.0f;
    f32 normalBias = 0.0f; // in texels (world offset = normalBias × texel size at the receiver)
    f32 nearPlane = 0.05f;
    f32 farPlane = 10.0f;
    u32 kind = 0;      // GpuShadowKind
    u32 cubeLayer = 0; // point: cube index in the cube array
    f32 texelSize = 0.0f; // world size of one texel at distance 1 (2·tan(fov/2) / resolution)
    f32 lightSize = 0.0f; // source radius in metres (PCSS)
};
static_assert(sizeof(GpuShadow) == 112);

// Per view, per frame. Matrices include the Vulkan Y flip (NDC y down, uv = ndc * 0.5 + 0.5) and reversed-Z.
struct GpuViewConstants {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f}; // jittered
    glm::mat4 viewProj{1.0f};
    glm::mat4 invView{1.0f};
    glm::mat4 invProj{1.0f};
    glm::mat4 invViewProj{1.0f};
    glm::mat4 unjitteredViewProj{1.0f};
    glm::mat4 prevUnjitteredViewProj{1.0f};
    glm::mat4 prevViewProj{1.0f};
    glm::vec4 cameraPosition{0.0f}; // xyz, w = time in seconds
    glm::vec4 renderSize{0.0f};     // xy = render resolution, zw = 1 / xy
    glm::vec4 outputSize{0.0f};
    glm::vec4 jitter{0.0f};         // xy = current jitter (NDC units), zw = previous
    f32 nearPlane = 0.1f;
    f32 farPlane = 0.0f; // 0 = infinite
    f32 preExposure = 1.0f; // SceneColorHDR = radiance * preExposure
    f32 exposure = 1.0f;    // full exposure (tonemapper multiplies by exposure / preExposure)
    f32 ev100 = 0.0f;
    f32 deltaTime = 0.0f;
    u32 frameIndex = 0;
    u32 debugView = 0;
    u32 flags = 0; // bit 0 orthographic, bit 1 editor
    u32 lightClusterCount = 0;
    f32 lodBias = 0.0f;
    f32 mipBias = 0.0f;

    // Clustered lighting (see clusters.glsl).
    glm::uvec4 clusterGrid{16, 9, 24, 256}; // x, y, z slices, max lights per cluster
    glm::vec4 clusterDepth{0.0f};            // x = scale, y = bias, z = near, w = far (slice = log(z)·x + y)
    u64 clusterBuffer = 0;                   // LightClusters buffer address

    // Sun + cascaded shadow maps.
    i32 sunLight = -1;             // index into the light buffer, -1 = none
    u32 cascadeCount = 0;
    glm::mat4 cascadeViewProj[kMaxCascades]{};
    glm::vec4 cascadeSplits{0.0f};     // view-space far distance of each cascade
    glm::vec4 cascadeTexelWorld{0.0f}; // world size of one shadow texel per cascade
    glm::vec4 cascadeDepthRange{0.0f}; // light-space depth range per cascade (PCSS)
    glm::vec4 shadowParams{0.0f};      // x = blend fraction, y = filter radius (texels), z = pcss, w = pcf taps
    u32 cascadeTexture = kInvalidIndex; // sampled index of ShadowCascades (2D array)
    u32 shadowAtlas = kInvalidIndex;
    u32 pointShadows = kInvalidIndex;   // cube array
    u32 shadowAtlasSize = 0;
    f32 sunBias = 0.0f;
    f32 sunNormalBias = 0.0f;
    f32 csmResolution = 0.0f;
    f32 sunAngularRadius = 0.0f; // radians (PCSS)

    // Environment / IBL.
    u64 irradianceSH = 0; // address of 9 × vec4 cosine-convolved SH (rgb), evaluate with oxEvalSH9; always valid
    u32 shPad0 = 0, shPad1 = 0;
    u32 prefilteredCube = kInvalidIndex;
    u32 brdfLut = kInvalidIndex;
    u32 skyCube = kInvalidIndex; // environment cubemap (sky capture or HDRI)
    u32 prefilteredMips = 1;
    f32 iblIntensity = 1.0f;
    f32 skyIntensity = 1.0f;
    u32 skyMode = 0; // 0 procedural, 1 cubemap, 2 Preetham
    u32 pad1 = 0;
    glm::vec4 fogColor{0.0f};  // rgb, w = enabled
    glm::vec4 fogParams{0.0f}; // density, height falloff, start distance, unused
    glm::vec4 preetham[8]{};   // world::PreethamSky::Gpu layout (skyMode 2)

    // Default textures for optional inputs.
    u32 whiteTexture = kInvalidIndex;
    u32 blackTexture = kInvalidIndex;
    u32 flatNormalTexture = kInvalidIndex;
    u32 pad2 = 0;
};

// Scene-wide buffer addresses for a frame. Passed as push constant `scene` (see common/scene.glsl).
struct GpuSceneHeader {
    u64 instances = 0;
    u64 materials = 0;
    u64 meshes = 0;
    u64 positions = 0;
    u64 attributes = 0;
    u64 skin = 0;
    u64 palettes = 0;
    u64 lights = 0;
    u64 shadows = 0;
    u64 meshlets = 0;
    u32 instanceCount = 0;
    u32 materialCount = 0;
    u32 lightCount = 0;            // total entries in `lights`
    u32 directionalLightCount = 0; // directional lights are stored first
    u32 shadowCount = 0;
    u32 meshCount = 0;
    u32 pad0 = 0, pad1 = 0;
};
static_assert(sizeof(GpuSceneHeader) == 112);

} // namespace ox::render
