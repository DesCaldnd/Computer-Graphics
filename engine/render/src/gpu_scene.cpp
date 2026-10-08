#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/render/gpu_resource_cache.hpp>
#include <oxwald/render/gpu_scene.hpp>
#include <oxwald/render/snapshot.hpp>
#include <oxwald/rhi/device.hpp>

#include <algorithm>
#include <cstring>

namespace ox::render {

// --- RangeAllocator ---

u64 RangeAllocator::allocate(u64 count) {
    if (count == 0) count = 1;
    for (usize i = 0; i < m_free.size(); ++i) {
        Range& r = m_free[i];
        if (r.count >= count) {
            const u64 off = r.offset;
            r.offset += count;
            r.count -= count;
            if (r.count == 0) m_free.erase(m_free.begin() + i);
            m_used += count;
            return off;
        }
    }
    if (m_top + count > m_capacity) return ~0ull;
    const u64 off = m_top;
    m_top += count;
    m_used += count;
    return off;
}

void RangeAllocator::free(u64 offset, u64 count) {
    if (count == 0) count = 1;
    m_used -= std::min(m_used, count);
    auto it = std::lower_bound(m_free.begin(), m_free.end(), offset,
                               [](const Range& r, u64 off) { return r.offset < off; });
    it = m_free.insert(it, {offset, count});
    // Coalesce with neighbours.
    if (it + 1 != m_free.end() && it->offset + it->count == (it + 1)->offset) {
        it->count += (it + 1)->count;
        m_free.erase(it + 1);
    }
    if (it != m_free.begin() && (it - 1)->offset + (it - 1)->count == it->offset) {
        (it - 1)->count += it->count;
        it = m_free.erase(it) - 1;
    }
    // Give the tail back to the bump pointer.
    if (it->offset + it->count == m_top) {
        m_top = it->offset;
        m_free.erase(it);
    }
}

void RangeAllocator::grow(u64 newCapacity) { m_capacity = std::max(m_capacity, newCapacity); }

// --- GrowableBuffer ---

void GrowableBuffer::init(rhi::Device& device, std::string name, u64 elementSize, u64 initialCapacity,
                          rhi::BufferUsage usage) {
    m_name = std::move(name);
    m_elementSize = elementSize;
    m_usage = usage | rhi::BufferUsage::TransferDst | rhi::BufferUsage::TransferSrc | rhi::BufferUsage::Storage;
    m_alloc = RangeAllocator(initialCapacity);
    m_buffer = device.createBuffer({initialCapacity * elementSize, m_usage, rhi::MemoryUsage::GpuOnly, m_name});
    m_address = device.address(m_buffer);
}

void GrowableBuffer::release(rhi::Device& device) {
    if (m_buffer) device.destroy(m_buffer);
    m_buffer = {};
    m_address = 0;
}

u64 GrowableBuffer::allocate(rhi::Device& device, u64 count) {
    u64 off = m_alloc.allocate(count);
    if (off != ~0ull) return off;
    const u64 oldCap = m_alloc.capacity();
    const u64 newCap = std::max(oldCap * 2, oldCap + count * 2);
    OX_LOG_INFO("render", "growing {} from {} to {} elements", m_name, oldCap, newCap);
    rhi::BufferHandle bigger = device.createBuffer({newCap * m_elementSize, m_usage, rhi::MemoryUsage::GpuOnly, m_name});
    // Pending transfer-queue uploads may target the old buffer: let them land before copying.
    device.wait(device.flushUploads());
    const rhi::BufferHandle old = m_buffer;
    const u64 bytes = oldCap * m_elementSize;
    device.immediateSubmit([&](rhi::CommandList& cmd) {
        cmd.memoryBarrier(rhi::Access::General, rhi::Access::TransferRead);
        if (bytes) cmd.copyBuffer(old, bigger, bytes);
        cmd.memoryBarrier(rhi::Access::TransferWrite, rhi::Access::General);
    });
    device.destroy(old);
    m_buffer = bigger;
    m_address = device.address(bigger);
    m_alloc.grow(newCap);
    off = m_alloc.allocate(count);
    OX_ASSERT(off != ~0ull, "allocation after growth failed");
    return off;
}

// --- GpuScene ---

GpuScene::GpuScene(rhi::Device& device) : m_device(&device) {
    using U = rhi::BufferUsage;
    m_positions.init(device, "scene.positions", sizeof(glm::vec3), 256 * 1024, U::Storage | U::AccelStructInput);
    m_attributes.init(device, "scene.attributes", sizeof(assets::VertexAttributes), 256 * 1024, U::Storage);
    m_indices.init(device, "scene.indices", sizeof(u32), 1024 * 1024, U::Index | U::AccelStructInput);
    m_skin.init(device, "scene.skin", sizeof(assets::SkinVertex), 16 * 1024, U::Storage);
    m_meshlets.init(device, "scene.meshlets", sizeof(u32), 256 * 1024, U::Storage);
    m_meshInfoBuffer.init(device, "scene.meshInfos", sizeof(GpuMeshInfo), 4096, U::Storage);
    m_materialBuffer.init(device, "scene.materials", sizeof(GpuMaterial), 1024, U::Storage);
    m_instanceBuffer.init(device, "scene.instances", sizeof(GpuInstance), 16 * 1024, U::Storage);
    m_meshLodBuffer.init(device, "scene.meshLods", sizeof(GpuMeshLod) * kMaxMeshLods, 4096, U::Storage);
    m_meshInfos.resize(m_meshInfoBuffer.capacity());
    m_meshLods.resize(m_meshLodBuffer.capacity() * kMaxMeshLods);
    // Slot 0 = default material (white, roughness 0.5).
    const u32 def = allocateMaterial();
    OX_ASSERT(def == 0);
    setMaterial(def, GpuMaterial{});
}

GpuScene::~GpuScene() {
    for (GrowableBuffer* b : {&m_positions, &m_attributes, &m_indices, &m_skin, &m_meshlets, &m_meshInfoBuffer,
                              &m_materialBuffer, &m_instanceBuffer, &m_meshLodBuffer}) {
        b->release(*m_device);
    }
}

bool GpuScene::uploadMesh(const assets::MeshData& mesh, GpuMesh& out) {
    OX_PROFILE_ZONE();
    rhi::Device& dev = *m_device;
    const u32 vcount = u32(mesh.positions.size());
    if (vcount == 0 || mesh.attributes.size() != vcount || mesh.indices.empty() || mesh.submeshes.empty()) return false;
    out = {};
    out.vertexCount = vcount;
    out.vertexOffset = m_positions.allocate(dev, vcount);
    const u64 attrOffset = m_attributes.allocate(dev, vcount);
    OX_ASSERT(attrOffset == out.vertexOffset, "vertex streams out of sync");
    out.indexCount = u32(mesh.indices.size());
    out.indexOffset = m_indices.allocate(dev, out.indexCount);
    auto bytes = [](const auto& v) {
        return std::span<const u8>(reinterpret_cast<const u8*>(v.data()), v.size() * sizeof(v[0]));
    };
    dev.uploadBufferAsync(m_positions.buffer(), bytes(mesh.positions), out.vertexOffset * sizeof(glm::vec3),
                          rhi::Access::General);
    dev.uploadBufferAsync(m_attributes.buffer(), bytes(mesh.attributes),
                          out.vertexOffset * sizeof(assets::VertexAttributes), rhi::Access::General);
    dev.uploadBufferAsync(m_indices.buffer(), bytes(mesh.indices), out.indexOffset * sizeof(u32), rhi::Access::General);
    if (!mesh.skin.empty() && mesh.skin.size() == vcount) {
        out.skinOffset = m_skin.allocate(dev, vcount);
        dev.uploadBufferAsync(m_skin.buffer(), bytes(mesh.skin), out.skinOffset * sizeof(assets::SkinVertex),
                              rhi::Access::General);
    }
    if (!mesh.meshlets.empty()) {
        // Meshlet arena (u32 units): [Meshlet × n (16 u32 each)] [meshletVertices] [meshletTriangles, padded to 4].
        const u64 mWords = mesh.meshlets.size() * sizeof(assets::Meshlet) / 4;
        const u64 vWords = mesh.meshletVertices.size();
        const u64 tWords = (mesh.meshletTriangles.size() + 3) / 4;
        std::vector<u32> packed(mWords + vWords + tWords, 0);
        std::memcpy(packed.data(), mesh.meshlets.data(), mWords * 4);
        std::memcpy(packed.data() + mWords, mesh.meshletVertices.data(), vWords * 4);
        std::memcpy(packed.data() + mWords + vWords, mesh.meshletTriangles.data(), mesh.meshletTriangles.size());
        out.meshletOffset = m_meshlets.allocate(dev, packed.size());
        out.meshletCount = u32(mesh.meshlets.size());
        dev.uploadBufferAsync(m_meshlets.buffer(), bytes(packed), out.meshletOffset * 4, rhi::Access::General);
    }
    out.submeshCount = u32(mesh.submeshes.size());
    out.firstMeshInfo = u32(m_meshInfoBuffer.allocate(dev, out.submeshCount));
    const u64 lodOffset = m_meshLodBuffer.allocate(dev, out.submeshCount);
    OX_ASSERT(lodOffset == out.firstMeshInfo, "mesh LOD table out of sync");
    if (m_meshInfos.size() < m_meshInfoBuffer.capacity()) m_meshInfos.resize(m_meshInfoBuffer.capacity());
    if (m_meshLods.size() < m_meshLodBuffer.capacity() * kMaxMeshLods) m_meshLods.resize(m_meshLodBuffer.capacity() * kMaxMeshLods);
    ++m_structureVersion;
    const u64 meshletWords = mesh.meshlets.size() * sizeof(assets::Meshlet) / 4;
    out.bounds = mesh.bounds;
    out.boundingSphere = mesh.boundingSphere;
    for (u32 s = 0; s < out.submeshCount; ++s) {
        const assets::Submesh& sm = mesh.submeshes[s];
        GpuMeshInfo mi;
        const assets::MeshLod lod = sm.lods.empty() ? assets::MeshLod{0, out.indexCount, 0, 0, 0.0f} : sm.lods[0];
        mi.firstIndex = u32(out.indexOffset) + lod.indexOffset;
        mi.indexCount = lod.indexCount;
        mi.vertexOffset = i32(out.vertexOffset);
        mi.vertexCount = sm.vertexCount;
        AABB b = sm.bounds.valid() ? sm.bounds : mesh.bounds;
        if (!b.valid()) b = AABB::fromCenterExtents(glm::vec3(0.0f), glm::vec3(0.5f));
        mi.aabbMin = b.min;
        mi.aabbMax = b.max;
        mi.boundingSphere = glm::vec4(b.center(), glm::length(b.max - b.min) * 0.5f);
        mi.meshletOffset = out.meshletOffset == ~0ull ? kInvalidIndex : u32(out.meshletOffset);
        mi.meshletCount = 0;
        for (const assets::Meshlet& m : mesh.meshlets) mi.meshletCount += m.submesh == s ? 1u : 0u;
        mi.skinOffset = out.skinOffset == ~0ull ? kInvalidIndex : u32(out.skinOffset);
        mi.lodCount = std::clamp<u32>(u32(sm.lods.size()), 1, kMaxMeshLods);
        m_meshInfos[out.firstMeshInfo + s] = mi;
        GpuMeshLod* lods = &m_meshLods[usize(out.firstMeshInfo + s) * kMaxMeshLods];
        for (u32 l = 0; l < kMaxMeshLods; ++l) {
            GpuMeshLod gl;
            if (l < sm.lods.size() || (l == 0 && sm.lods.empty())) {
                const assets::MeshLod ml = sm.lods.empty() ? lod : sm.lods[l];
                gl.firstIndex = u32(out.indexOffset) + ml.indexOffset;
                gl.indexCount = ml.indexCount;
                gl.error = ml.error;
                if (out.meshletOffset != ~0ull && ml.meshletCount > 0) {
                    gl.meshletFirst = u32(out.meshletOffset) + ml.meshletOffset * u32(sizeof(assets::Meshlet) / 4);
                    gl.meshletCount = ml.meshletCount;
                    gl.meshletVertexBase = u32(out.meshletOffset + meshletWords);
                    gl.meshletTriangleBase = u32(out.meshletOffset + meshletWords + mesh.meshletVertices.size());
                }
            }
            lods[l] = gl;
        }
        m_dirtyMeshInfos.push_back(out.firstMeshInfo + s);
        out.submeshMaterialSlots.push_back(sm.materialSlot);
        out.slotMaterials.push_back(sm.materialSlot < mesh.materials.size() ? mesh.materials[sm.materialSlot].material : Uuid{});
        out.triangleCount += lod.indexCount / 3;
    }
    return true;
}

void GpuScene::freeMesh(GpuMesh& mesh) {
    if (mesh.vertexCount) {
        m_positions.free(mesh.vertexOffset, mesh.vertexCount);
        m_attributes.free(mesh.vertexOffset, mesh.vertexCount);
    }
    if (mesh.indexCount) m_indices.free(mesh.indexOffset, mesh.indexCount);
    if (mesh.skinOffset != ~0ull) m_skin.free(mesh.skinOffset, mesh.vertexCount);
    if (mesh.submeshCount) {
        m_meshInfoBuffer.free(mesh.firstMeshInfo, mesh.submeshCount);
        m_meshLodBuffer.free(mesh.firstMeshInfo, mesh.submeshCount);
    }
    ++m_structureVersion;
    // Meshlet ranges are not tracked by size here; they leak until the arena is rebuilt (rare: hot reload only).
    mesh = {};
}

u32 GpuScene::allocateMaterial() {
    if (!m_freeMaterials.empty()) {
        const u32 i = m_freeMaterials.back();
        m_freeMaterials.pop_back();
        return i;
    }
    const u32 i = u32(m_materialBuffer.allocate(*m_device, 1));
    if (m_materials.size() <= i) m_materials.resize(i + 1);
    return i;
}

void GpuScene::setMaterial(u32 index, const GpuMaterial& material) {
    if (m_materials.size() <= index) {
        m_materials.resize(index + 1);
        ++m_structureVersion;
    }
    constexpr u32 kBatchFlags = kMaterialBlendMask | kMaterialDoubleSided;
    if ((m_materials[index].flags & kBatchFlags) != (material.flags & kBatchFlags)) ++m_structureVersion;
    m_materials[index] = material;
    m_dirtyMaterials.push_back(index);
}

void GpuScene::freeMaterial(u32 index) {
    if (index == 0) return;
    m_freeMaterials.push_back(index);
}

void GpuScene::markInstanceDirty(u32 slot) {
    if (m_instanceDirtyFlag.size() <= slot) m_instanceDirtyFlag.resize(slot + 1, 0);
    if (!m_instanceDirtyFlag[slot]) {
        m_instanceDirtyFlag[slot] = 1;
        m_dirtyInstances.push_back(slot);
    }
}

namespace {

glm::vec4 worldSphere(const glm::mat4& m, const glm::vec4& local) {
    const glm::vec3 c = glm::vec3(m * glm::vec4(glm::vec3(local), 1.0f));
    const f32 s = std::sqrt(std::max({glm::dot(glm::vec3(m[0]), glm::vec3(m[0])), glm::dot(glm::vec3(m[1]), glm::vec3(m[1])),
                                      glm::dot(glm::vec3(m[2]), glm::vec3(m[2]))}));
    return {c, local.w * s};
}

} // namespace

void GpuScene::updateInstances(const RenderSnapshot& snapshot, GpuResourceCache& cache, f32 drawDistance,
                               const glm::vec3& cameraPosition) {
    OX_PROFILE_ZONE();
    ++m_updateStamp;
    m_movedBounds.clear();
    m_liveInstances = 0;
    m_movedInstances = 0;
    // Sorted scratch instead of a temporary hash set: no allocation in steady state (an empty std::unordered_set
    // already allocates its sentinel node and buckets on the MSVC STL).
    m_selectedScratch.assign(snapshot.selection.begin(), snapshot.selection.end());
    std::sort(m_selectedScratch.begin(), m_selectedScratch.end());
    for (const SnapshotMesh& sm : snapshot.meshes) {
        const GpuMesh* gm = cache.mesh(sm.mesh);
        if (!gm) continue;
        const bool moved = sm.world != sm.prevWorld;
        for (u32 s = 0; s < gm->submeshCount; ++s) {
            const InstanceKey key{sm.entityId, s};
            auto it = m_instanceSlots.find(key);
            u32 slot;
            if (it == m_instanceSlots.end()) {
                if (!m_freeInstances.empty()) {
                    slot = m_freeInstances.back();
                    m_freeInstances.pop_back();
                } else {
                    slot = u32(m_instances.size());
                    m_instances.emplace_back();
                    m_instanceSeen.push_back(0);
                    const u64 off = m_instanceBuffer.allocate(*m_device, 1);
                    OX_ASSERT(off == slot, "instance slots out of sync");
                }
                m_instanceSlots.emplace(key, slot);
                m_instances[slot].flags = ~0u; // force upload
                ++m_structureVersion;
            } else {
                slot = it->second;
            }
            m_instanceSeen[slot] = m_updateStamp;

            GpuInstance inst;
            inst.world = sm.world;
            inst.prevWorld = sm.prevWorld;
            inst.meshIndex = gm->firstMeshInfo + s;
            const GpuMeshInfo& mi = m_meshInfos[inst.meshIndex];
            inst.boundingSphere = worldSphere(sm.world, mi.boundingSphere);
            Uuid matId;
            if (s < sm.materialCount) matId = snapshot.materials[sm.materialOffset + s];
            else if (sm.materialCount > 0) matId = snapshot.materials[sm.materialOffset + sm.materialCount - 1];
            else if (s < gm->slotMaterials.size()) matId = gm->slotMaterials[s];
            inst.materialIndex = matId.isValid() ? cache.materialIndex(matId) : defaultMaterial();
            inst.flags = kInstanceVisible;
            if (sm.flags & kMeshCastShadows) inst.flags |= kInstanceCastShadows;
            if (sm.flags & kMeshReceiveShadows) inst.flags |= kInstanceReceiveShadows;
            if (moved) inst.flags |= kInstanceMoved;
            if (!m_selectedScratch.empty() &&
                std::binary_search(m_selectedScratch.begin(), m_selectedScratch.end(), sm.entityId)) {
                inst.flags |= kInstanceSelected;
            }
            if (sm.paletteOffset != ~0u && mi.skinOffset != kInvalidIndex) {
                inst.flags |= kInstanceSkinned;
                inst.paletteOffset = sm.paletteOffset;
                inst.prevPaletteOffset = sm.prevPaletteOffset != ~0u ? sm.prevPaletteOffset : sm.paletteOffset;
                u32 cur = 0, prev = 0;
                if (m_skinResolver && m_skinResolver(sm, *gm, s, m_updateStamp, cur, prev)) {
                    inst.flags |= kInstanceSkinnedOutput;
                    inst.paletteOffset = cur;
                    inst.prevPaletteOffset = prev;
                }
                // Skinned bounds are not known: inflate conservatively.
                inst.boundingSphere.w *= 2.0f;
                inst.flags |= kInstanceMoved;
            }
            inst.entityId = sm.entityId;
            {
                const GpuInstance& old = m_instances[slot];
                constexpr u32 kBatchFlags = kInstanceVisible | kInstanceCastShadows;
                if (old.meshIndex != inst.meshIndex || old.materialIndex != inst.materialIndex ||
                    (old.flags & kBatchFlags) != (inst.flags & kBatchFlags)) {
                    ++m_structureVersion;
                }
            }
            if (std::memcmp(&inst, &m_instances[slot], sizeof(GpuInstance)) != 0) {
                m_instances[slot] = inst;
                markInstanceDirty(slot);
            }
            ++m_liveInstances;
            if (inst.flags & kInstanceMoved) {
                ++m_movedInstances;
                m_movedBounds.push_back({glm::vec3(inst.boundingSphere), inst.boundingSphere.w});
                const glm::vec4 prev = worldSphere(sm.prevWorld, mi.boundingSphere);
                m_movedBounds.push_back({glm::vec3(prev), prev.w * ((inst.flags & kInstanceSkinned) ? 2.0f : 1.0f)});
            }
        }
    }
    // Free slots of entities that disappeared.
    for (auto it = m_instanceSlots.begin(); it != m_instanceSlots.end();) {
        const u32 slot = it->second;
        if (m_instanceSeen[slot] != m_updateStamp) {
            if (m_instances[slot].flags & kInstanceCastShadows) {
                m_movedBounds.push_back({glm::vec3(m_instances[slot].boundingSphere), m_instances[slot].boundingSphere.w});
            }
            m_instances[slot] = GpuInstance{};
            m_instances[slot].flags = 0;
            markInstanceDirty(slot);
            m_freeInstances.push_back(slot);
            it = m_instanceSlots.erase(it);
            ++m_structureVersion;
        } else {
            ++it;
        }
    }
    (void)drawDistance;
    (void)cameraPosition;
}

bool GpuScene::hasPendingUploads() const {
    return !m_dirtyInstances.empty() || !m_dirtyMaterials.empty() || !m_dirtyMeshInfos.empty();
}

void GpuScene::recordUploads(rhi::CommandList& cmd, const std::function<GpuAllocation(u64)>& allocate) {
    if (!hasPendingUploads()) return;
    OX_PROFILE_ZONE();
    struct Copy {
        rhi::BufferHandle dst;
        u64 srcOffset, dstOffset, size;
        rhi::BufferHandle src;
    };
    std::vector<Copy> copies;
    auto stage = [&](std::vector<u32>& dirty, const void* base, u64 stride, rhi::BufferHandle dst) {
        if (dirty.empty()) return;
        std::sort(dirty.begin(), dirty.end());
        dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
        usize i = 0;
        while (i < dirty.size()) {
            usize j = i + 1;
            while (j < dirty.size() && dirty[j] == dirty[j - 1] + 1) ++j;
            const u64 first = dirty[i], count = dirty[j - 1] - dirty[i] + 1;
            GpuAllocation a = allocate(count * stride);
            std::memcpy(a.cpu, static_cast<const u8*>(base) + first * stride, count * stride);
            copies.push_back({dst, a.offset, first * stride, count * stride, a.buffer});
            i = j;
        }
        dirty.clear();
    };
    stage(m_dirtyInstances, m_instances.data(), sizeof(GpuInstance), m_instanceBuffer.buffer());
    std::fill(m_instanceDirtyFlag.begin(), m_instanceDirtyFlag.end(), 0);
    stage(m_dirtyMaterials, m_materials.data(), sizeof(GpuMaterial), m_materialBuffer.buffer());
    std::vector<u32> dirtyLods = m_dirtyMeshInfos;
    stage(dirtyLods, m_meshLods.data(), sizeof(GpuMeshLod) * kMaxMeshLods, m_meshLodBuffer.buffer());
    stage(m_dirtyMeshInfos, m_meshInfos.data(), sizeof(GpuMeshInfo), m_meshInfoBuffer.buffer());
    // Previous frames (same queue) may still read these buffers: order the copies after them.
    cmd.memoryBarrier(rhi::Access::General, rhi::Access::TransferWrite);
    for (const Copy& c : copies) cmd.copyBuffer(c.src, c.dst, c.size, c.srcOffset, c.dstOffset);
    cmd.memoryBarrier(rhi::Access::TransferWrite, rhi::Access::General);
}

void GpuScene::fillHeader(GpuSceneHeader& h) const {
    h.instances = m_instanceBuffer.address();
    h.materials = m_materialBuffer.address();
    h.meshes = m_meshInfoBuffer.address();
    h.positions = m_positions.address();
    h.attributes = m_attributes.address();
    h.skin = m_skin.address();
    h.meshlets = m_meshlets.address();
    h.skinnedVertices = m_skinnedVertices;
    h.instanceCount = u32(m_instances.size());
    h.materialCount = u32(m_materials.size());
    h.meshCount = u32(m_meshInfoBuffer.capacity());
}

namespace {

DrawBucket bucketFor(const GpuMaterial& m) {
    switch (m.flags & kMaterialBlendMask) {
    case 1: return DrawBucket::Masked;
    case 2: return DrawBucket::Transparent;
    case 3: return DrawBucket::Refractive;
    default: return DrawBucket::Opaque;
    }
}

struct SortItem {
    u64 key;
    u32 instance;
    f32 depth;
    u32 lod;
};

// Batch key: variant (2 bits) | LOD (3) | mesh index (27) | material (32).
u64 batchKey(u32 variant, u32 lod, u32 mesh, u32 material) {
    return (u64(variant) << 62) | (u64(lod & 7u) << 59) | (u64(mesh & 0x7FFFFFFu) << 32) | u64(material);
}

void groupBatches(std::vector<SortItem>& items, bool sortBackToFront, const std::vector<GpuInstance>& instances,
                  const std::vector<GpuMeshInfo>& meshes, const std::vector<GpuMeshLod>& lods,
                  const std::vector<GpuMaterial>& materials, DrawBucket bucket, DrawList& out, std::vector<u32>& ids) {
    if (sortBackToFront) {
        std::sort(items.begin(), items.end(), [](const SortItem& a, const SortItem& b) { return a.depth > b.depth; });
    } else {
        std::sort(items.begin(), items.end(), [](const SortItem& a, const SortItem& b) {
            return a.key != b.key ? a.key < b.key : a.instance < b.instance;
        });
    }
    for (usize i = 0; i < items.size();) {
        usize j = i + 1;
        if (!sortBackToFront) {
            while (j < items.size() && items[j].key == items[i].key) ++j;
        }
        const GpuInstance& inst = instances[items[i].instance];
        const GpuMeshInfo& mi = meshes[inst.meshIndex];
        const GpuMeshLod& ml = lods[usize(inst.meshIndex) * kMaxMeshLods + items[i].lod];
        const GpuMaterial& mat = materials[inst.materialIndex];
        DrawBatch b;
        b.meshIndex = inst.meshIndex;
        b.materialIndex = inst.materialIndex;
        b.firstInstance = u32(ids.size());
        b.instanceCount = u32(j - i);
        b.firstIndex = ml.firstIndex;
        b.indexCount = ml.indexCount;
        b.vertexOffset = mi.vertexOffset;
        b.variant = (bucket == DrawBucket::Masked ? kVariantAlphaTest : 0u) |
                    ((mat.flags & kMaterialDoubleSided) ? kVariantDoubleSided : 0u);
        b.sortDepth = items[i].depth;
        for (usize k = i; k < j; ++k) ids.push_back(items[k].instance);
        out.batches.push_back(b);
        out.instanceCount += b.instanceCount;
        out.triangleCount += u64(ml.indexCount / 3) * b.instanceCount;
        i = j;
    }
}

} // namespace

struct GpuScene::DrawListScratch {
    std::vector<SortItem> items[u32(DrawBucket::Count)];
};

void GpuScene::buildDrawList(const DrawFilter& filter, DrawList& out, std::vector<u32>& ids, DrawBucket only) const {
    OX_PROFILE_ZONE();
    out = {};
    if (!m_scratch) m_scratch = std::make_unique<DrawListScratch>();
    auto& items = m_scratch->items;
    for (auto& v : items) v.clear();
    for (u32 i = 0; i < m_instances.size(); ++i) {
        const GpuInstance& inst = m_instances[i];
        if (!(inst.flags & kInstanceVisible)) continue;
        if ((inst.flags & filter.requiredInstanceFlags) != filter.requiredInstanceFlags) continue;
        const DrawBucket bucket = bucketFor(m_materials[inst.materialIndex]);
        if (!(filter.bucketMask & (1u << u32(bucket)))) continue;
        if (only != DrawBucket::Count && bucket != only) continue;
        const Sphere sphere{glm::vec3(inst.boundingSphere), inst.boundingSphere.w};
        if (filter.frustum && !filter.frustum->intersects(sphere)) continue;
        if (filter.sphere && !filter.sphere->intersects(sphere)) continue;
        const u32 variant = (bucket == DrawBucket::Masked ? 1u : 0u) |
                            ((m_materials[inst.materialIndex].flags & kMaterialDoubleSided) ? 2u : 0u);
        const u32 lod = filter.lod ? instanceLod(inst, *filter.lod) : 0u;
        // Shadow lists batch by mesh only (material only matters for alpha test).
        const u64 key = batchKey(variant, lod, inst.meshIndex, bucket == DrawBucket::Masked ? inst.materialIndex : 0u);
        items[u32(bucket)].push_back({key, i, glm::length(glm::vec3(inst.boundingSphere) - filter.sortOrigin), lod});
    }
    for (u32 b = 0; b < u32(DrawBucket::Count); ++b) {
        if (items[b].empty()) continue;
        groupBatches(items[b], false, m_instances, m_meshInfos, m_meshLods, m_materials, DrawBucket(b), out, ids);
    }
}

void GpuScene::buildViewDrawLists(const Frustum& frustum, const glm::vec3& cameraPos, ViewDrawLists& out,
                                  std::vector<u32> (&ids)[u32(DrawBucket::Count)], f32 drawDistance,
                                  bool frustumCulling, const LodSelection* lodSel) const {
    OX_PROFILE_ZONE();
    if (!m_scratch) m_scratch = std::make_unique<DrawListScratch>();
    auto& items = m_scratch->items;
    for (auto& v : items) v.clear();
    for (u32 i = 0; i < m_instances.size(); ++i) {
        const GpuInstance& inst = m_instances[i];
        if (!(inst.flags & kInstanceVisible)) continue;
        const Sphere sphere{glm::vec3(inst.boundingSphere), inst.boundingSphere.w};
        if (frustumCulling && !frustum.intersects(sphere)) continue;
        const f32 dist = glm::length(sphere.center - cameraPos);
        if (drawDistance > 0.0f && dist - sphere.radius > drawDistance) continue;
        const GpuMaterial& mat = m_materials[inst.materialIndex];
        const DrawBucket bucket = bucketFor(mat);
        const u32 variant = (bucket == DrawBucket::Masked ? 1u : 0u) | ((mat.flags & kMaterialDoubleSided) ? 2u : 0u);
        const u32 lod = lodSel ? instanceLod(inst, *lodSel) : 0u;
        const u64 key = batchKey(variant, lod, inst.meshIndex, inst.materialIndex);
        items[u32(bucket)].push_back({key, i, dist, lod});
    }
    for (u32 b = 0; b < u32(DrawBucket::Count); ++b) {
        DrawList& l = out.buckets[b]; // reuse capacity
        l.batches.clear();
        l.instanceIds = 0;
        l.instanceCount = 0;
        l.triangleCount = 0;
        ids[b].clear();
        const bool backToFront = b == u32(DrawBucket::Transparent) || b == u32(DrawBucket::Refractive);
        groupBatches(items[b], backToFront, m_instances, m_meshInfos, m_meshLods, m_materials, DrawBucket(b),
                     out.buckets[b], ids[b]);
    }
}

u32 GpuScene::instanceLod(const GpuInstance& inst, const LodSelection& s) const {
    const GpuMeshInfo& mi = m_meshInfos[inst.meshIndex];
    if (mi.lodCount <= 1 || s.projScale <= 0.0f) return 0;
    const f32 meshRadius = std::max(mi.boundingSphere.w, 1e-6f);
    const f32 scale = inst.boundingSphere.w / meshRadius;
    const f32 dist = glm::length(glm::vec3(inst.boundingSphere) - s.cameraPosition) - inst.boundingSphere.w;
    return selectLod(meshLods(inst.meshIndex), mi.lodCount, scale, dist, s);
}

GpuScene::Stats GpuScene::stats() const {
    Stats s;
    s.positionBytes = m_positions.used() * m_positions.elementSize();
    s.attributeBytes = m_attributes.used() * m_attributes.elementSize();
    s.indexBytes = m_indices.used() * 4;
    s.materials = u32(m_materials.size() - m_freeMaterials.size());
    s.instances = m_liveInstances;
    return s;
}

} // namespace ox::render
