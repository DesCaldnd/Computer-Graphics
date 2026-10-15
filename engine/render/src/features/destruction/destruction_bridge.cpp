#include "destruction_bridge.hpp"

#include <oxwald/render/render_feature.hpp>

#if OX_RENDER_HAS_GAMEPLAY

#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/destruction.hpp>
#include <oxwald/render/gpu_resource_cache.hpp>
#include <oxwald/render/mesh_primitives.hpp>
#include <oxwald/render/renderer.hpp>
#include <oxwald/render/snapshot.hpp>
#include <oxwald/scene/world.hpp>

#include <mutex>
#include <unordered_set>

namespace ox::render {

namespace {

struct DestructionMeshSnapshot final : ISnapshotExtension {
    std::vector<std::shared_ptr<const gameplay::PreparedFracture>> fractures;
    void clear() override { fractures.clear(); }
};

void extractDestruction(const World& world, RenderSnapshot& out) {
    const entt::registry& r = world.registry();
    // Few distinct fractures (many chunks share one): a linear search over the snapshot's vector deduplicates
    // without allocating — the vector keeps its capacity across frames (steady-state extract is allocation-free).
    DestructionMeshSnapshot* ext = nullptr;
    auto add = [&](const std::shared_ptr<const gameplay::PreparedFracture>& p) {
        if (!p) return;
        if (!ext) ext = &out.extension<DestructionMeshSnapshot>();
        for (const auto& f : ext->fractures) {
            if (f.get() == p.get()) return;
        }
        ext->fractures.push_back(p);
    };
    for (auto [e, d] : r.view<const gameplay::DestructibleComponent>().each()) add(d.prepared);
    for (auto [e, c] : r.view<const gameplay::DestructionChunkComponent>().each()) add(c.fracture);
}

assets::MeshData chunkMesh(const physics::FractureChunk& c, const std::string& name) {
    assets::MeshData m;
    m.name = name;
    m.positions = c.positions;
    m.attributes.resize(c.positions.size());
    for (usize i = 0; i < c.positions.size(); ++i) {
        m.attributes[i].normal = c.normals[i];
        m.attributes[i].uv0 = c.uvs[i];
        m.attributes[i].uv1 = c.uvs[i];
    }
    m.indices = c.indices;
    auto submesh = [&](u32 first, u32 count, u32 slot, const char* part) {
        if (count == 0) return;
        assets::Submesh sm;
        sm.name = name + "." + part;
        sm.materialSlot = slot;
        sm.vertexOffset = 0;
        sm.vertexCount = u32(m.positions.size());
        assets::MeshLod lod;
        lod.indexOffset = first;
        lod.indexCount = count;
        sm.lods.push_back(lod);
        m.submeshes.push_back(sm);
    };
    const u32 total = u32(c.indices.size());
    const u32 outer = std::min(c.interiorIndexStart, total);
    // Slot order matches the chunk MeshRenderer: outer surface, then cut faces (a chunk without outer faces has
    // only the interior submesh in slot 0).
    submesh(0, outer, 0, "outer");
    submesh(outer, total - outer, outer > 0 ? 1u : 0u, "interior");
    m.materials.push_back({"outer", Uuid{}});
    if (outer > 0 && total > outer) m.materials.push_back({"interior", Uuid{}});
    computeTangents(m);
    computeBounds(m);
    return m;
}

class DestructionMeshFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "DestructionMeshes"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::PreDepth); }
    void setup(FeatureContext& ctx) override {
        const auto* ext = ctx.snapshot().findExtension<DestructionMeshSnapshot>();
        if (!ext || ext->fractures.empty()) return;
        GpuResourceCache* cache = nullptr;
        for (const auto& f : ext->fractures) {
            if (!f->result || m_uploaded.contains(f->key)) continue;
            OX_PROFILE_ZONE_N("DestructionMeshes::upload");
            if (!cache) cache = &ctx.renderer().resources();
            for (u32 i = 0; i < f->meshes.size() && i < f->result->chunks.size(); ++i) {
                cache->addMesh(f->meshes[i], chunkMesh(f->result->chunks[i], "ox.destruction.chunk" + std::to_string(i)));
            }
            m_uploaded.insert(f->key);
        }
    }

private:
    std::unordered_set<u64> m_uploaded; // fracture keys already registered in this renderer's cache
};

} // namespace

void registerDestructionFeatures(FeatureRegistry& features) {
    static std::once_flag once;
    std::call_once(once, [] { addExtractHook(&extractDestruction); });
    features.add(std::make_unique<DestructionMeshFeature>());
}

} // namespace ox::render

#else

namespace ox::render {
void registerDestructionFeatures(FeatureRegistry&) {}
} // namespace ox::render

#endif
