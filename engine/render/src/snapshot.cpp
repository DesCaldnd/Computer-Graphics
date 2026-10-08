#include <oxwald/core/jobs.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/render/snapshot.hpp>
#include <oxwald/scene/world.hpp>

#include <algorithm>
#include <mutex>

namespace ox::render {

void RenderSnapshot::clear() {
    frame = 0;
    time = 0.0;
    deltaTime = 0.0f;
    cameras.clear();
    meshes.clear();
    materials.clear();
    lights.clear();
    palettes.clear();
    environment.reset();
    debugLines.clear();
    debugLinesOverlay.clear();
    selection.clear();
    for (auto& [type, ext] : extensions) {
        if (ext) ext->clear();
    }
}

i32 RenderSnapshot::primaryCamera() const {
    for (usize i = 0; i < cameras.size(); ++i) {
        if (cameras[i].camera.primary) return i32(i);
    }
    return cameras.empty() ? -1 : 0;
}

namespace {

std::mutex& hookMutex() {
    static std::mutex m;
    return m;
}
std::vector<ExtractHook>& hooks() {
    static std::vector<ExtractHook> h;
    return h;
}
std::vector<ExtractHookEx>& hooksEx() {
    static std::vector<ExtractHookEx> h;
    return h;
}

bool activeInHierarchy(const entt::registry& reg, entt::entity e) {
    for (u32 depth = 0; e != entt::null && depth < 1024; ++depth) {
        if (const auto* a = reg.try_get<ActiveComponent>(e); a && !a->active) return false;
        const auto* h = reg.try_get<HierarchyComponent>(e);
        e = h ? h->parent : entt::entity{entt::null};
    }
    return true;
}

u32 entityId(entt::entity e) { return encodeEntityId(u32(entt::to_integral(e))); }

} // namespace

void addExtractHook(ExtractHook hook) {
    std::lock_guard lock(hookMutex());
    hooks().push_back(hook);
}

void addExtractHookEx(ExtractHookEx hook) {
    std::lock_guard lock(hookMutex());
    if (std::find(hooksEx().begin(), hooksEx().end(), hook) == hooksEx().end()) hooksEx().push_back(hook);
}

namespace {

// Mesh renderers: serial, or two parallel passes (count → prefix sums → write) into pre-sized arrays. Steady state
// allocates nothing: the scratch arrays are thread-local and keep their capacity, the snapshot keeps its own.
template <class Visible>
void extractMeshes(const entt::registry& reg, RenderSnapshot& out, const ExtractOptions& options, Visible&& visible) {
    OX_PROFILE_ZONE();
    auto view = reg.view<const MeshRendererComponent, const WorldTransformComponent>();
    auto write = [&](entt::entity e, const MeshRendererComponent& mr, const WorldTransformComponent& wt, SnapshotMesh& m,
                     Uuid* materials, u32 materialOffset) {
        m.entityId = entityId(e);
        m.mesh = mr.mesh;
        m.materialOffset = materialOffset;
        m.materialCount = u32(mr.materials.size());
        std::copy(mr.materials.begin(), mr.materials.end(), materials);
        m.world = wt.matrix;
        m.prevWorld = wt.previous;
        m.flags = (mr.castShadows ? kMeshCastShadows : 0u) | (mr.receiveShadows ? kMeshReceiveShadows : 0u);
        m.layerMask = mr.layerMask;
    };
    thread_local std::vector<entt::entity> entities;
    entities.clear();
    for (entt::entity e : view) entities.push_back(e);
    const u32 n = u32(entities.size());
    if (!options.jobs || n < options.parallelThreshold) {
        for (entt::entity e : entities) {
            const auto& mr = view.get<const MeshRendererComponent>(e);
            if (!mr.visible || !mr.mesh.isValid() || !visible(e)) continue;
            const auto& wt = view.get<const WorldTransformComponent>(e);
            const u32 matOffset = u32(out.materials.size());
            out.materials.resize(matOffset + mr.materials.size());
            out.meshes.emplace_back();
            write(e, mr, wt, out.meshes.back(), out.materials.data() + matOffset, matOffset);
        }
        return;
    }
    // Pass 1: per chunk, accepted meshes and materials (accepted flag per entity).
    const u32 grain = 512;
    const u32 chunks = (n + grain - 1) / grain;
    thread_local std::vector<u8> accepted;
    thread_local std::vector<u32> chunkMeshes, chunkMaterials;
    accepted.resize(n);
    chunkMeshes.assign(chunks + 1, 0);
    chunkMaterials.assign(chunks + 1, 0);
    u8* acc = accepted.data();
    u32* cm = chunkMeshes.data();
    u32* cmat = chunkMaterials.data();
    const entt::entity* ents = entities.data();
    options.jobs->parallelFor(chunks, 1, [&](u32 begin, u32 end, u32) {
        for (u32 c = begin; c < end; ++c) {
            u32 meshes = 0, materials = 0;
            for (u32 i = c * grain; i < std::min(n, (c + 1) * grain); ++i) {
                const auto& mr = view.get<const MeshRendererComponent>(ents[i]);
                acc[i] = mr.visible && mr.mesh.isValid() && visible(ents[i]) ? 1 : 0;
                meshes += acc[i];
                materials += acc[i] ? u32(mr.materials.size()) : 0u;
            }
            cm[c + 1] = meshes;
            cmat[c + 1] = materials;
        }
    });
    for (u32 c = 0; c < chunks; ++c) {
        cm[c + 1] += cm[c];
        cmat[c + 1] += cmat[c];
    }
    const u32 meshBase = u32(out.meshes.size()), materialBase = u32(out.materials.size());
    out.meshes.resize(meshBase + cm[chunks]);
    out.materials.resize(materialBase + cmat[chunks]);
    SnapshotMesh* meshes = out.meshes.data() + meshBase;
    Uuid* materials = out.materials.data() + materialBase;
    // Pass 2: write (same order as the serial path).
    options.jobs->parallelFor(chunks, 1, [&](u32 begin, u32 end, u32) {
        for (u32 c = begin; c < end; ++c) {
            u32 mi = cm[c], mat = cmat[c];
            for (u32 i = c * grain; i < std::min(n, (c + 1) * grain); ++i) {
                if (!acc[i]) continue;
                const auto& mr = view.get<const MeshRendererComponent>(ents[i]);
                const auto& wt = view.get<const WorldTransformComponent>(ents[i]);
                write(ents[i], mr, wt, meshes[mi], materials + mat, materialBase + mat);
                ++mi;
                mat += u32(mr.materials.size());
            }
        }
    });
}

} // namespace

void extract(const World& world, RenderSnapshot& out, const ExtractOptions& options) {
    OX_PROFILE_ZONE();
    std::vector<u32> selection;
    selection.swap(out.selection); // keep the editor selection (and its capacity) across clear()
    out.clear();
    out.selection.swap(selection);
    out.frame = options.frame;
    out.time = options.time;
    out.deltaTime = options.deltaTime;
    const entt::registry& reg = world.registry();
    auto visible = [&](entt::entity e) { return options.includeInactive || activeInHierarchy(reg, e); };

    for (auto [e, cam, wt] : reg.view<const CameraComponent, const WorldTransformComponent>().each()) {
        if (!visible(e)) continue;
        out.cameras.push_back({entityId(e), cam, wt.matrix, wt.previous});
    }

    extractMeshes(reg, out, options, visible);

    i32 brightestSun = -1;
    f32 brightest = -1.0f;
    for (auto [e, light, wt] : reg.view<const LightComponent, const WorldTransformComponent>().each()) {
        if (!visible(e)) continue;
        SnapshotLight l;
        l.entityId = entityId(e);
        l.light = light;
        l.position = glm::vec3(wt.matrix[3]);
        const glm::vec3 fwd = -glm::vec3(wt.matrix[2]);
        l.direction = glm::length(fwd) > 1e-6f ? glm::normalize(fwd) : glm::vec3(0, 0, -1);
        l.moved = wt.matrix != wt.previous;
        if (light.type == LightType::Directional && light.intensity > brightest) {
            brightest = light.intensity;
            brightestSun = i32(out.lights.size());
        }
        out.lights.push_back(l);
    }

    for (auto [e, env] : reg.view<const EnvironmentComponent>().each()) {
        if (!visible(e)) continue;
        SnapshotEnvironment se;
        se.environment = env;
        se.sunLight = brightestSun;
        if (env.sun.valid()) {
            if (Entity sunEntity = world.resolve(env.sun)) {
                const u32 id = entityId(sunEntity.handle());
                for (usize i = 0; i < out.lights.size(); ++i) {
                    if (out.lights[i].entityId == id && out.lights[i].light.type == LightType::Directional) {
                        se.sunLight = i32(i);
                    }
                }
            }
        }
        out.environment = se;
        break;
    }

    if (options.debugDraw) {
        auto d = options.debugDraw->depthTestedLines();
        out.debugLines.assign(d.begin(), d.end());
        auto o = options.debugDraw->overlayLines();
        out.debugLinesOverlay.assign(o.begin(), o.end());
    }

    thread_local std::vector<ExtractHook> hs; // copies outside the lock; capacity is kept (no per-frame allocation)
    thread_local std::vector<ExtractHookEx> hsEx;
    {
        std::lock_guard lock(hookMutex());
        hs.assign(hooks().begin(), hooks().end());
        hsEx.assign(hooksEx().begin(), hooksEx().end());
    }
    for (ExtractHook h : hs) h(world, out);
    for (ExtractHookEx h : hsEx) h(world, out, options);
}

void SnapshotBuffer::publish() {
    std::lock_guard lock(m_mutex);
    m_read.store(m_write);
    m_write = 1 - m_write;
}

const RenderSnapshot& SnapshotBuffer::readSlot() const {
    std::lock_guard lock(m_mutex);
    return m_slots[m_read.load()];
}

} // namespace ox::render
