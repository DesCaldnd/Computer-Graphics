#pragma once

// DDGI (Majercik et al. 2019) probe volume used by the ray traced GI feature. The reflections-ao team did not ship a
// probe-volume layout yet, so this one is defined here and documented in render.md ("Ray tracing" → DDGI layout).
//
// Layout
//   * Probes sit on an infinite world grid: probe world coordinate c (ivec3) is at position (c + 0.5) * spacing.
//     The volume is a window of `counts` probes starting at `minCoord` (snapped around the camera). It scrolls
//     with the camera; probe storage is addressed modulo the counts, so probes that stay inside the window keep
//     their data and only the newly entered slices are reset (detected per probe through the probe state buffer).
//   * storage index s = sx + sy * counts.x + sz * counts.x * counts.y with (sx, sy, sz) = c mod counts.
//   * Irradiance atlas: RGBA16F, one (N + 2)² tile per probe (N = 8 octahedral texels + 1 texel border), tiles laid
//     out `counts.x * counts.y` per row, `counts.z` rows. Value = cosine-weighted mean radiance = irradiance / π
//     (exactly the "diffuse radiance of a white surface" of the IndirectDiffuse contract).
//   * Depth atlas: RG16F, (16 + 2)² tiles, mean distance and mean squared distance (Chebyshev visibility).
//   * Ray buffer: RGBA16F, raysPerProbe × probeCount: rgb radiance, a = hit distance (negative = back face).
//   * Probe state buffer: ivec4 per storage slot: xyz = world coordinate currently stored, w = valid flag.
// GLSL: engine/shaders/render/raytracing/ddgi.glsl (oxDdgiIrradiance for sampling, from any shader).

#include <oxwald/core/math.hpp>
#include <oxwald/core/types.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/rhi/handles.hpp>

namespace ox::render::rt {

inline constexpr u32 kDdgiIrradianceTexels = 8;
inline constexpr u32 kDdgiDepthTexels = 16;

struct DdgiVolumeDesc {
    glm::ivec3 counts{24, 8, 24};
    f32 spacing = 2.0f;
    u32 raysPerProbe = 128;
};

// Mirrors `DdgiVolume` in ddgi.glsl (scalar layout, 96 bytes).
struct DdgiVolumeGpu {
    glm::ivec3 minCoord{0};
    f32 spacing = 2.0f;
    glm::ivec3 counts{0};
    u32 raysPerProbe = 0;
    u32 irradianceTexture = ~0u; // bindless sampled index
    u32 depthTexture = ~0u;
    u32 enabled = 0;
    u32 frame = 0;
    f32 normalBias = 0.25f;  // × spacing... in metres, offset along the surface normal before sampling
    f32 viewBias = 0.25f;
    f32 hysteresis = 0.97f;
    f32 depthSharpness = 50.0f;
    glm::vec4 rayRotation{0.0f, 0.0f, 0.0f, 1.0f}; // quaternion applied to the spherical Fibonacci directions
    u64 probeState = 0;  // address of ivec4[probeCount]
    u32 pad0 = 0, pad1 = 0;
};
static_assert(sizeof(DdgiVolumeGpu) == 96);

[[nodiscard]] inline u32 ddgiProbeCount(const glm::ivec3& counts) { return u32(counts.x * counts.y * counts.z); }

// Window origin so the camera sits in the middle cell (snapped to whole probes).
[[nodiscard]] glm::ivec3 ddgiMinCoord(const glm::ivec3& counts, f32 spacing, const glm::vec3& cameraPosition);
[[nodiscard]] glm::vec3 ddgiProbePosition(const glm::ivec3& worldCoord, f32 spacing);
// Storage slot of a world probe coordinate (modular addressing, stable while the probe stays in the window).
[[nodiscard]] u32 ddgiStorageIndex(const glm::ivec3& worldCoord, const glm::ivec3& counts);
// World coordinate stored in a slot for a window starting at minCoord.
[[nodiscard]] glm::ivec3 ddgiWorldCoord(u32 storageIndex, const glm::ivec3& minCoord, const glm::ivec3& counts);
[[nodiscard]] glm::uvec2 ddgiAtlasSize(const glm::ivec3& counts, u32 texels);
// Top-left texel of the tile *interior* (the 1-texel border is around it).
[[nodiscard]] glm::uvec2 ddgiTileInterior(u32 storageIndex, const glm::ivec3& counts, u32 texels);
// Spherical Fibonacci direction i of n (before the per-frame rotation).
[[nodiscard]] glm::vec3 ddgiRayDirection(u32 i, u32 n);

// GPU side of a DDGI volume: persistent atlases + probe state, the probe update passes (blend rays into the
// octahedral tiles with hysteresis, border copy, scrolling resets) and the screen-space apply pass. The ray tracing
// step (ddgi_trace.comp) belongs to the GI feature; everything here is plain compute and runs on any device (the
// tests feed synthetic rays).
class DdgiVolumeRenderer {
public:
    bool initialize(rhi::Device& device);
    void shutdown(rhi::Device& device);
    // (Re)allocates the atlases / state when the layout changes (probes restart).
    void configure(rhi::Device& device, const DdgiVolumeDesc& desc);
    // Volume constants for this frame: window around `camera`, random ray rotation from `frame`.
    [[nodiscard]] DdgiVolumeGpu frameVolume(rhi::Device& device, const glm::vec3& camera, u32 frame) const;
    // Transient RGBA16F ray texture (raysPerProbe × probeCount) to be filled by a trace pass.
    rhi::RGTexture createRayTexture(FeatureContext& ctx) const;
    // Imported persistent atlases for this graph (call once per frame and view before declaring passes).
    void import(FeatureContext& ctx);
    [[nodiscard]] rhi::RGTexture irradiance() const { return m_irradianceRG; }
    [[nodiscard]] rhi::RGTexture depth() const { return m_depthRG; }
    // Probe update from a filled ray texture. `volume`: device address of this frame's DdgiVolumeGpu.
    void update(FeatureContext& ctx, rhi::RGTexture rays, VkDeviceAddress volume);
    // IndirectDiffuse (RGBA16F, irradiance / π) at `extent` (= render extent / scale).
    rhi::RGTexture apply(FeatureContext& ctx, VkDeviceAddress volume, rhi::RGTexture depth, rhi::RGTexture normals,
                         Extent2D extent, u32 scale);
    [[nodiscard]] const DdgiVolumeDesc& desc() const { return m_desc; }
    [[nodiscard]] u32 probeCount() const { return ddgiProbeCount(m_desc.counts); }
    [[nodiscard]] rhi::TextureHandle irradianceTexture() const { return m_irradiance; }
    [[nodiscard]] rhi::TextureHandle depthTexture() const { return m_depth; }
    [[nodiscard]] rhi::BufferHandle probeStateBuffer() const { return m_state; }

private:
    DdgiVolumeDesc m_desc{glm::ivec3(0), 0.0f, 0};
    rhi::TextureHandle m_irradiance, m_depth;
    rhi::BufferHandle m_state;
    rhi::RGTexture m_irradianceRG, m_depthRG;
    rhi::RGBuffer m_stateRG;
    rhi::PipelineHandle m_updateIrradiance, m_updateDepth, m_border, m_apply;
};

} // namespace ox::render::rt
