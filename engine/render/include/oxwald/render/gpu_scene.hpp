#pragma once

// GpuScene: persistent GPU-side scene data shared by all views.
//   * geometry arenas (positions vec3, attributes 48 B, u32 indices, skin, meshlets) — vertex pulling via BDA;
//     one index buffer for everything, so any draw (including future GPU-driven indirect draws) binds it once
//   * mesh table (GpuMeshInfo per submesh)
//   * material table (GpuMaterial)
//   * instance table (GpuInstance per (entity, submesh)), stable slots, incremental updates from the snapshot
// Updates are staged on the CPU and copied on the graphics queue by the first pass of the frame ("SceneUpload").

#include <oxwald/assets/mesh.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/render/gpu_types.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/rhi/handles.hpp>

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ox::rhi {
class Device;
class CommandList;
} // namespace ox::rhi

namespace ox::render {

struct RenderSnapshot;
class GpuResourceCache;

// Suballocation of a growable GPU buffer, in elements.
class RangeAllocator {
public:
    explicit RangeAllocator(u64 capacity = 0) : m_capacity(capacity) {}
    // Returns ~0ull when it does not fit.
    u64 allocate(u64 count);
    void free(u64 offset, u64 count);
    void grow(u64 newCapacity);
    [[nodiscard]] u64 capacity() const { return m_capacity; }
    [[nodiscard]] u64 used() const { return m_used; }

private:
    struct Range {
        u64 offset;
        u64 count;
    };
    std::vector<Range> m_free; // sorted by offset, coalesced
    u64 m_capacity = 0;
    u64 m_top = 0;
    u64 m_used = 0;
};

// A device buffer that grows (copying its contents) when an allocation does not fit.
class GrowableBuffer {
public:
    void init(rhi::Device& device, std::string name, u64 elementSize, u64 initialCapacity, rhi::BufferUsage usage);
    void release(rhi::Device& device);
    // Returns the element offset; grows the buffer (synchronous copy, rare) when needed.
    u64 allocate(rhi::Device& device, u64 count);
    void free(u64 offset, u64 count) { m_alloc.free(offset, count); }
    [[nodiscard]] rhi::BufferHandle buffer() const { return m_buffer; }
    [[nodiscard]] VkDeviceAddress address() const { return m_address; }
    [[nodiscard]] u64 elementSize() const { return m_elementSize; }
    [[nodiscard]] u64 capacity() const { return m_alloc.capacity(); }
    [[nodiscard]] u64 used() const { return m_alloc.used(); }

private:
    std::string m_name;
    rhi::BufferHandle m_buffer;
    VkDeviceAddress m_address = 0;
    u64 m_elementSize = 1;
    rhi::BufferUsage m_usage{};
    RangeAllocator m_alloc;
};

// Uploaded mesh: a contiguous range of GpuMeshInfo entries (one per submesh).
struct GpuMesh {
    u32 firstMeshInfo = 0;
    u32 submeshCount = 0;
    u64 vertexOffset = 0;
    u32 vertexCount = 0;
    u64 indexOffset = 0;
    u32 indexCount = 0;
    u64 skinOffset = ~0ull;
    u64 meshletOffset = ~0ull;
    u32 meshletCount = 0;
    AABB bounds;
    Sphere boundingSphere;
    std::vector<u32> submeshMaterialSlots;
    std::vector<Uuid> slotMaterials; // per submesh: the mesh's own material (used when the renderer has none)
    u64 triangleCount = 0;
};

class GpuScene {
public:
    explicit GpuScene(rhi::Device& device);
    ~GpuScene();
    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    rhi::Device& device() { return *m_device; }

    // --- geometry (render thread) ---
    // Uploads through the transfer queue (Device::uploadBufferAsync). Returns false for empty / invalid data.
    bool uploadMesh(const assets::MeshData& mesh, GpuMesh& out);
    void freeMesh(GpuMesh& mesh);
    [[nodiscard]] const GpuMeshInfo& meshInfo(u32 index) const { return m_meshInfos[index]; }
    [[nodiscard]] rhi::BufferHandle indexBuffer() const { return m_indices.buffer(); }

    // --- materials ---
    u32 allocateMaterial();
    void setMaterial(u32 index, const GpuMaterial& material);
    void freeMaterial(u32 index);
    [[nodiscard]] const GpuMaterial& material(u32 index) const { return m_materials[index]; }
    [[nodiscard]] u32 defaultMaterial() const { return 0; }

    // --- instances (once per frame, before any view) ---
    // Resolves meshes/materials through the cache, assigns stable slots per (entity, submesh), stages changes.
    void updateInstances(const RenderSnapshot& snapshot, GpuResourceCache& cache, f32 drawDistance,
                         const glm::vec3& cameraPosition);
    [[nodiscard]] u32 instanceCount() const { return u32(m_instances.size()); }
    [[nodiscard]] const std::vector<GpuInstance>& instances() const { return m_instances; }
    [[nodiscard]] u32 liveInstanceCount() const { return m_liveInstances; }
    [[nodiscard]] u32 movedInstanceCount() const { return m_movedInstances; }
    // World-space bounding spheres of instances that moved this frame (current and previous positions).
    [[nodiscard]] const std::vector<Sphere>& movedBounds() const { return m_movedBounds; }

    // Copies staged instance/material/mesh-table changes from per-frame upload memory (`allocate`). Records into
    // `cmd` (graphics queue) including the barriers against previous frames' reads.
    void recordUploads(rhi::CommandList& cmd, const std::function<GpuAllocation(u64)>& allocate);
    [[nodiscard]] bool hasPendingUploads() const;

    // Fills the persistent part of a scene header (lights/shadows are added per view).
    void fillHeader(GpuSceneHeader& header) const;

    // Culls instances and groups them into instanced batches.
    // bucket = DrawBucket::Count: every bucket allowed by filter.bucketMask.
    void buildDrawList(const DrawFilter& filter, DrawList& out, std::vector<u32>& instanceIdsOut,
                       DrawBucket bucket = DrawBucket::Count) const;
    [[nodiscard]] const GpuMeshInfo* meshInfos() const { return m_meshInfos.data(); }
    // All four buckets at once (camera view).
    void buildViewDrawLists(const Frustum& frustum, const glm::vec3& cameraPos, ViewDrawLists& out,
                            std::vector<u32> (&instanceIds)[u32(DrawBucket::Count)], f32 drawDistance,
                            bool frustumCulling = true) const;

    struct Stats {
        u64 positionBytes = 0, attributeBytes = 0, indexBytes = 0;
        u32 meshes = 0, materials = 0, instances = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    struct InstanceKey {
        u32 entity;
        u32 submesh;
        bool operator==(const InstanceKey&) const = default;
    };
    struct KeyHash {
        size_t operator()(const InstanceKey& k) const noexcept { return (u64(k.entity) << 16) ^ k.submesh; }
    };
    void markInstanceDirty(u32 slot);

    rhi::Device* m_device;
    GrowableBuffer m_positions, m_attributes, m_indices, m_skin, m_meshlets;
    GrowableBuffer m_meshInfoBuffer, m_materialBuffer, m_instanceBuffer;
    std::vector<GpuMeshInfo> m_meshInfos;
    RangeAllocator m_meshInfoAlloc{1u << 20};
    std::vector<GpuMaterial> m_materials;
    std::vector<u32> m_freeMaterials;
    std::vector<GpuInstance> m_instances;
    std::vector<u32> m_freeInstances;
    std::vector<u64> m_instanceSeen; // frame stamp per slot
    std::unordered_map<InstanceKey, u32, KeyHash> m_instanceSlots;
    std::vector<u32> m_dirtyInstances, m_dirtyMaterials, m_dirtyMeshInfos;
    std::vector<u8> m_instanceDirtyFlag;
    std::vector<Sphere> m_movedBounds;
    u32 m_liveInstances = 0;
    u32 m_movedInstances = 0;
    u64 m_updateStamp = 0;
};

} // namespace ox::render
