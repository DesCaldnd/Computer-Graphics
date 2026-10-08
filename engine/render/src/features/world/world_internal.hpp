#pragma once

// Private declarations of the world-skinning area (features/world/*.cpp).

#include "../../renderer_impl.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/render/features/world/world_skinning.hpp>

#include <functional>
#include <memory>

namespace ox::render::worldfx {

// --- cvars (world_registration.cpp) ---
struct WorldCVars {
    // Foliage
    f32 foliageDensity = 1.0f;
    f32 foliageDrawDistanceScale = 1.0f;
    f32 foliageImpostorDistanceScale = 1.0f;
    bool foliageGrass = true;
    bool foliageShadows = true;
    bool foliageImpostors = true;
    bool foliage = true;
    // Terrain
    bool terrain = true;
    f32 terrainLodScale = 1.0f;
    i32 terrainMaxLayers = 8;
    bool terrainTriplanar = true;
    bool terrainShadows = true;
    bool terrainTessellation = false;
    // Sky
    bool aerialPerspective = true;
    f32 aerialDensity = 1.0f;
    f32 iblUpdateDegrees = 1.0f;
    i32 skyCubeSize = 128;
    // Skinning
    bool computeSkinning = true;
    static WorldCVars read();
};
std::vector<std::string> worldGeometryCVarNames();
std::vector<std::string> worldSkyCVarNames();
std::vector<std::string> skinningCVarNames();

// --- shared by the geometry features ---

// Per-view data a geometry system prepared for the current frame (captured by pass lambdas).
struct WorldPassContext {
    VkDeviceAddress view = 0;
    VkDeviceAddress scene = 0;
    VkDeviceAddress matrix = 0; // shadow: cascade view-projection (one mat4)
    u32 cascade = 0;
    bool entityIds = false;
    u32 lightDirOct = 0; // shadow: sun travel direction (packOctUnorm) for light-facing impostors
    u32 inputs[4] = {kInvalidIndex, kInvalidIndex, kInvalidIndex, kInvalidIndex};
    rhi::BufferHandle sceneIndexBuffer;
};

class TerrainSystem;
class VegetationSystem;

// Terrain + vegetation, shared by the "WorldGeometry" (AfterDepth prepass + AfterOpaque forward) and "WorldShadows"
// (Shadows: cascades + ShadowMask) features.
struct WorldGeometryShared {
    std::unique_ptr<TerrainSystem> terrain;
    std::unique_ptr<VegetationSystem> vegetation;
    u64 updatedFrame = ~0ull;
    rhi::PipelineHandle hiz, shadowMask;
    bool initialized = false;
    bool terrainReady = false, vegetationReady = false;
    rhi::Device* m_device = nullptr;
    Renderer* m_renderer = nullptr;
    u32 users = 0;
    ~WorldGeometryShared();
    bool initialize(rhi::Device& device, Renderer& renderer);
    void shutdown(rhi::Device& device);
    // Once per renderer frame (first view): uploads, prototype resolution, impostor bakes.
    void update(FeatureContext& ctx, const WorldCVars& cv);
};

std::unique_ptr<IRenderFeature> makeWorldGeometryFeature(std::shared_ptr<WorldGeometryShared> shared);
std::unique_ptr<IRenderFeature> makeWorldShadowsFeature(std::shared_ptr<WorldGeometryShared> shared);
std::unique_ptr<IRenderFeature> makeWorldSkyFeature();
std::unique_ptr<IRenderFeature> makeSkinningFeature();

// Render-only extract hook (VegetationPrototypesComponent, TerrainRenderComponent).
void extractWorldComponents(const World& world, RenderSnapshot& out);
// Gameplay bridge (gameplay_bridge.cpp): WorldRenderData + SkinnedMeshComponent → snapshot. No-op without gameplay.
void registerGameplayBridge();

// Octahedral encoding matching oxOctEncode (math.glsl), packed as unorm16x2.
u32 packOctUnorm(glm::vec3 n);

} // namespace ox::render::worldfx
