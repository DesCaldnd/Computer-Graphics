#include <oxwald/core/profile.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/render/snapshot.hpp>
#include <oxwald/scene/world.hpp>

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

void extract(const World& world, RenderSnapshot& out, const ExtractOptions& options) {
    OX_PROFILE_ZONE();
    const std::vector<u32> selection = std::move(out.selection);
    out.clear();
    out.selection = selection;
    out.frame = options.frame;
    out.time = options.time;
    out.deltaTime = options.deltaTime;
    const entt::registry& reg = world.registry();
    auto visible = [&](entt::entity e) { return options.includeInactive || activeInHierarchy(reg, e); };

    for (auto [e, cam, wt] : reg.view<const CameraComponent, const WorldTransformComponent>().each()) {
        if (!visible(e)) continue;
        out.cameras.push_back({entityId(e), cam, wt.matrix, wt.previous});
    }

    for (auto [e, mr, wt] : reg.view<const MeshRendererComponent, const WorldTransformComponent>().each()) {
        if (!mr.visible || !mr.mesh.isValid() || !visible(e)) continue;
        SnapshotMesh m;
        m.entityId = entityId(e);
        m.mesh = mr.mesh;
        m.materialOffset = u32(out.materials.size());
        m.materialCount = u32(mr.materials.size());
        out.materials.insert(out.materials.end(), mr.materials.begin(), mr.materials.end());
        m.world = wt.matrix;
        m.prevWorld = wt.previous;
        m.flags = (mr.castShadows ? kMeshCastShadows : 0u) | (mr.receiveShadows ? kMeshReceiveShadows : 0u);
        m.layerMask = mr.layerMask;
        out.meshes.push_back(m);
    }

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

    std::vector<ExtractHook> hs;
    {
        std::lock_guard lock(hookMutex());
        hs = hooks();
    }
    for (ExtractHook h : hs) h(world, out);
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
