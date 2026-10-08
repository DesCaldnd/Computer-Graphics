#pragma once

// Terrain and vegetation renderers (private to the world-skinning area). Both are driven by the WorldGeometry /
// WorldShadows features (world_geometry.cpp): once per frame update(), per view prepare*() on the CPU (selection,
// culling dispatch declaration) and draw*() inside the merged passes.

#include "world_internal.hpp"

#if OX_RENDER_HAS_WORLD

#include <oxwald/render/gpu_scene.hpp>

#include <array>
#include <map>
#include <unordered_map>

namespace ox::render::worldfx {

enum class WorldPass : u8 { Prepass, Forward, Shadow };

// ----------------------------------------------------------------------------------------------------------------
// Terrain
// ----------------------------------------------------------------------------------------------------------------

class TerrainSystem {
public:
    bool initialize(rhi::Device& device);
    void shutdown(rhi::Device& device);
    void update(FeatureContext& ctx, const WorldSnapshot* world, const WorldCVars& cv);

    struct Draw {
        VkDeviceAddress params = 0;
        VkDeviceAddress patches = 0;
        VkDeviceAddress grid = 0;
        rhi::BufferHandle indexBuffer;
        u32 fullCount = 0;
        u32 fullIndexCount = 0;
        std::array<u32, 4> quadrantCount{};
        std::array<u32, 4> quadrantFirstInstance{};
        std::array<world::IndexRange, 4> quadrants{};
        u64 triangles = 0;
        bool tessellated = false;
    };
    struct ViewData {
        std::vector<Draw> draws;
        [[nodiscard]] bool empty() const { return draws.empty(); }
    };
    // Selects patches for a camera (and optionally a culling frustum other than the camera's, for cascades) and uploads
    // patch + parameter data to frame memory. Returns null when there is nothing to draw.
    std::shared_ptr<ViewData> prepare(FeatureContext& ctx, const glm::vec3& lodCamera, const glm::mat4& cullViewProj,
                                      bool shadow, const WorldCVars& cv);
    void draw(rhi::CommandList& cmd, const ViewData& data, WorldPass pass, const WorldPassContext& pc,
              FeatureContext& fc) const;
    [[nodiscard]] bool hasTerrain() const { return !m_terrains.empty(); }

private:
    struct GridGpu {
        rhi::BufferHandle vertices, indices;
        u32 indexCount = 0;
        std::array<world::IndexRange, 4> quadrants{};
        u32 gridDim = 0;
    };
    struct TerrainGpu {
        u32 entityId = 0;
        std::shared_ptr<const world::Heightfield> heightfield;
        u64 heightVersion = ~0ull;
        u64 splatVersion = ~0ull;
        world::HeightfieldDesc desc{};
        rhi::TextureHandle height, normal, holes, splat0, splat1;
        bool hasHoles = false;
        u32 splatLayers = 0;
        u32 splatResolution = 0;
        glm::vec2 splatOrigin{0.0f};
        f32 splatSize = 0.0f;
        std::unique_ptr<world::TerrainQuadtree> quadtree;
        world::TerrainLodSettings lodBuilt{};
        f32 lodScaleBuilt = 0.0f;
        std::vector<f32> skirt;
        std::vector<Uuid> layerMaterials;
        TerrainRenderComponent settings{};
        u64 lastSeen = 0;
    };
    void uploadHeight(rhi::Device& dev, TerrainGpu& t, const TerrainSnapshot& s);
    void uploadSplat(rhi::Device& dev, TerrainGpu& t, const TerrainSnapshot& s);
    void destroy(rhi::Device& dev, TerrainGpu& t);
    GridGpu& grid(rhi::Device& dev, u32 gridDim);

    rhi::PipelineHandle m_normals;
    std::unordered_map<u32, TerrainGpu> m_terrains;
    std::map<u32, GridGpu> m_grids;
    u64 m_frame = 0;

public:
    // Pipelines: [prepass, prepass + entity id, forward, shadow]
    rhi::PipelineHandle pipePrepass[2], pipeForward, pipeShadow;
    rhi::PipelineHandle tessPrepass[2], tessForward; // only with DeviceCaps::tessellationShader
};

// ----------------------------------------------------------------------------------------------------------------
// Vegetation
// ----------------------------------------------------------------------------------------------------------------

// Built-in procedural prototypes (vegetation_assets.cpp): meshes / materials registered in the resource cache.
struct BuiltinVegetation {
    Uuid treeLods[3], grassLods[3], bushLods[3];
    f32 treeHeight = 8.0f, grassHeight = 0.55f, bushHeight = 1.2f;
    f32 treeRadius = 4.3f, grassRadius = 0.35f, bushRadius = 0.9f;
};
const BuiltinVegetation& registerBuiltinVegetation(GpuResourceCache& cache);

class VegetationSystem {
public:
    bool initialize(rhi::Device& device, Renderer& renderer);
    void shutdown(rhi::Device& device);
    void update(FeatureContext& ctx, const WorldSnapshot* world, const WorldCVars& cv);

    struct Command {
        u32 proto = 0;      // prototype slot (VegProto index)
        u32 lod = 0;        // 0..2 mesh, 3 impostor
        u32 material = 0;   // GpuMaterial index
        u32 variant = 0;    // DrawVariant bits
        u64 triangles = 0;
    };
    // One culling job (main view or one cascade): its graph buffers and the CPU side of the indirect commands.
    struct CullJob {
        rhi::RGBuffer records, indirect;
        u64 argsOffset = 0;  // byte offset of the VkDrawIndexedIndirectCommands in `indirect`
        std::vector<Command> commands;
        bool empty() const { return commands.empty(); }
    };
    struct ViewData {
        std::vector<CullJob> jobs; // [0] = main view, or one per cascade (shadow)
        VkDeviceAddress frame = 0; // VegFrame
        VkDeviceAddress protos = 0;
        u32 entityId = 0;
    };
    // Declares the culling compute pass(es) (must be called before the passes that draw them).
    std::shared_ptr<ViewData> prepare(FeatureContext& ctx, const glm::vec3& lodCamera,
                                      std::span<const glm::mat4> cullViewProjs, bool shadow, const WorldCVars& cv);
    void declareReads(rhi::PassBuilder& pass, const ViewData& data) const;
    void draw(rhi::PassContext& p, const ViewData& data, u32 job, WorldPass pass, const WorldPassContext& pc,
              FeatureContext& fc) const;
    // Records impostor bakes queued by update() (graph pass declared by the WorldGeometry feature).
    [[nodiscard]] bool hasPendingBakes() const { return !m_pendingBakes.empty(); }
    void declareBakes(FeatureContext& ctx);
    [[nodiscard]] bool hasVegetation() const { return m_instanceCount > 0; }

private:
    struct MeshLod {
        const GpuMesh* mesh = nullptr;
        std::vector<u32> materials; // per submesh
        std::vector<u32> variants;
    };
    struct Proto {
        u32 prototype = 0;
        world::VegetationKind kind = world::VegetationKind::Grass;
        MeshLod lods[3];
        bool impostor = false;
        rhi::TextureHandle albedoAtlas, normalAtlas;
        bool baked = false;
        u32 impostorFrames = 8;
        f32 radius = 1.0f, centerY = 0.5f, height = 1.0f;
        f32 windSway = 1.0f, windFlutter = 1.0f, translucency = 0.6f;
        bool castShadows = true;
        u64 meshKey = 0; // changes when the mesh set changes (re-bake)
    };
    struct LayerSlot {
        u32 proto = 0;
        world::VegetationLodSettings lod{};
        bool castsShadow = false;
        world::VegetationKind kind{};
        bool impostors = false;
        f32 density = 1.0f;
        bool enabled = true;
    };
    struct BatchGpu {
        u64 version = ~0ull;
        u64 offset = 0; // arena element offset
        u32 count = 0;
        std::shared_ptr<const std::vector<world::VegetationCell>> cells;
        std::vector<u32> cellLayerSlot; // per cell
        u64 lastSeen = 0;
    };
    u32 protoSlot(GpuResourceCache& cache, u16 prototype, world::VegetationKind kind, const WorldSnapshot& world);
    void resolveMeshes(GpuResourceCache& cache, GpuScene& scene, Proto& p, const VegetationPrototypeDesc* desc);

    rhi::Device* m_device = nullptr;
    Renderer* m_renderer = nullptr;
    const BuiltinVegetation* m_builtin = nullptr;
    GrowableBuffer m_arena; // world::VegetationInstanceGpu
    std::unordered_map<u64, BatchGpu> m_batches;
    std::vector<std::pair<u64, std::pair<u64, u32>>> m_pendingFrees; // frame, (offset, count)
    std::vector<Proto> m_protos;
    std::map<std::pair<u32, u32>, u32> m_protoIndex; // (prototype, kind) → slot
    std::vector<LayerSlot> m_layers;
    std::vector<u32> m_activeBatches; // not used externally
    std::vector<u64> m_frameBatches;  // batch keys of this frame
    std::vector<u32> m_pendingBakes;  // proto slots
    u64 m_frame = 0;
    u64 m_instanceCount = 0;
    glm::vec4 m_frameWind[2]{}, m_prevWind[2]{};
    std::vector<glm::vec4> m_interactors;
    f32 m_time = 0.0f, m_prevTime = 0.0f;
    bool m_hasWind = false;
    rhi::BufferHandle m_quadIndices;
    rhi::PipelineHandle m_cull, m_args, m_bake;

public:
    // [alpha test][entity id]
    rhi::PipelineHandle pipePrepass[2][2], pipeForward[2], pipeShadow[2];
    rhi::PipelineHandle impPrepass[2], impForward, impShadow;
};

} // namespace ox::render::worldfx

#endif // OX_RENDER_HAS_WORLD
