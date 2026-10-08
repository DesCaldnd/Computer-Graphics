// Components (reflection + registry), extract hook and CPU Gerstner helpers of the translucency area.
#include <oxwald/core/reflect.hpp>
#include <oxwald/render/components/translucency.hpp>
#include <oxwald/render/features/translucency/translucency.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/components.hpp>
#include <oxwald/scene/world.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace ox::render {

// water_bridge.cpp (gameplay WorldRenderData water when gameplay is linked).
bool waterOwnedByGameplay(const World& world, u32 enttValue);
void installTranslucencyWaterBridge();

namespace {

constexpr f32 kTwoPi = 6.28318530718f;
constexpr f32 kGravity = 9.81f;

bool activeInHierarchy(const entt::registry& reg, entt::entity e) {
    for (u32 depth = 0; e != entt::null && depth < 1024; ++depth) {
        if (const auto* a = reg.try_get<ActiveComponent>(e); a && !a->active) return false;
        const auto* h = reg.try_get<HierarchyComponent>(e);
        e = h ? h->parent : entt::entity{entt::null};
    }
    return true;
}

const std::vector<WaterWave>& defaultSwell() {
    static const std::vector<WaterWave> waves = {
        {{1.0f, 0.2f}, 14.0f, 0.18f, 0.55f, 0.0f, 1.0f},
        {{0.7f, -0.6f}, 7.5f, 0.09f, 0.5f, 1.3f, 1.0f},
        {{-0.3f, 1.0f}, 4.1f, 0.05f, 0.45f, 2.1f, 1.0f},
        {{0.9f, 0.8f}, 2.3f, 0.025f, 0.4f, 4.0f, 1.0f},
    };
    return waves;
}

void extractTranslucency(const World& world, RenderSnapshot& out) {
    const entt::registry& reg = world.registry();
    TranslucencySnapshot* data = nullptr;
    auto get = [&]() -> TranslucencySnapshot& {
        if (!data) data = &out.extension<TranslucencySnapshot>();
        return *data;
    };
    const f32 time = f32(out.time);
    for (auto [e, emitter, wt] : reg.view<const ParticleEmitterComponent, const WorldTransformComponent>().each()) {
        if (!emitter.enabled || !activeInHierarchy(reg, e)) continue;
        SnapshotParticleEmitter s;
        s.entityId = encodeEntityId(u32(entt::to_integral(e)));
        s.world = wt.matrix;
        s.emitter = emitter;
        get().emitters.push_back(std::move(s));
    }
    for (auto [e, water, wt] : reg.view<const WaterSurfaceComponent, const WorldTransformComponent>().each()) {
        if (!water.visible || !activeInHierarchy(reg, e)) continue;
        if (waterOwnedByGameplay(world, u32(entt::to_integral(e)))) continue; // drawn by the WorldRenderData bridge
        SnapshotWater s;
        s.entityId = encodeEntityId(u32(entt::to_integral(e)));
        const glm::vec3 pos(wt.matrix[3]);
        s.params = packGerstnerWaves(water.waves.empty() ? defaultSwell() : water.waves, pos.y, time);
        s.center = {pos.x, pos.z};
        s.size = water.size;
        s.look = water;
        s.look.waves.clear();
        get().water.push_back(std::move(s));
    }
}

std::once_flag g_hookOnce;

} // namespace

void installTranslucencyExtractHook() {
    std::call_once(g_hookOnce, [] {
        addExtractHook(&extractTranslucency);
        installTranslucencyWaterBridge();
    });
}

void addWaterSurface(RenderSnapshot& snapshot, const SnapshotWater& water) {
    snapshot.extension<TranslucencySnapshot>().water.push_back(water);
}

GerstnerParams packGerstnerWaves(const std::vector<WaterWave>& waves, f32 baseHeight, f32 time) {
    GerstnerParams p;
    const u32 count = u32(std::min<usize>(waves.size(), 16));
    for (u32 i = 0; i < count; ++i) {
        const WaterWave& w = waves[i];
        const glm::vec2 d = glm::dot(w.direction, w.direction) > 1e-12f ? glm::normalize(w.direction) : glm::vec2(1.0f, 0.0f);
        const f32 k = kTwoPi / std::max(w.wavelength, 1e-3f);
        const f32 omega = std::sqrt(kGravity * k) * w.speedScale;
        const f32 qa = std::clamp(w.steepness, 0.0f, 1.0f) / (k * f32(count));
        p.waves[i] = {{d.x, d.y, k, omega}, {w.amplitude, qa, w.phase, 0.0f}};
    }
    p.info = {f32(count), baseHeight, time, 0.0f};
    return p;
}

glm::vec3 gerstnerDisplacement(const GerstnerParams& p, glm::vec2 x0, f32 t) {
    glm::vec3 r(0.0f);
    const u32 count = std::min(u32(p.info.x), 16u);
    for (u32 i = 0; i < count; ++i) {
        const auto& w = p.waves[i];
        const glm::vec2 d(w.dirK.x, w.dirK.y);
        const f32 theta = w.dirK.z * glm::dot(d, x0) - w.dirK.w * t + w.amp.z;
        const f32 c = std::cos(theta), s = std::sin(theta);
        r.x += w.amp.y * d.x * c;
        r.z += w.amp.y * d.y * c;
        r.y += w.amp.x * s;
    }
    return r;
}

f32 gerstnerHeight(const GerstnerParams& p, glm::vec2 xz, f32 t, u32 iterations) {
    glm::vec2 x0 = xz;
    for (u32 i = 0; i < iterations; ++i) {
        const glm::vec3 d = gerstnerDisplacement(p, x0, t);
        x0 = xz - glm::vec2(d.x, d.z);
    }
    return p.info.y + gerstnerDisplacement(p, x0, t).y;
}

f32 gerstnerAmplitudeSum(const GerstnerParams& p) {
    f32 s = 0.0f;
    const u32 count = std::min(u32(p.info.x), 16u);
    for (u32 i = 0; i < count; ++i) s += std::abs(p.waves[i].amp.x);
    return s;
}

void registerTranslucencyTypes() {
    using attr::AssetRef;
    using attr::Category;
    using attr::Color;
    using attr::Meta;
    using attr::Range;
    using attr::Step;
    using attr::Tooltip;

    OX_REFLECT_ENUM(ParticleShape, "ParticleShape")
        .value("Point", ParticleShape::Point)
        .value("Sphere", ParticleShape::Sphere)
        .value("Cone", ParticleShape::Cone)
        .value("Box", ParticleShape::Box)
        .value("MeshSurface", ParticleShape::MeshSurface);
    OX_REFLECT_ENUM(ParticleBlend, "ParticleBlend")
        .value("Additive", ParticleBlend::Additive)
        .value("Alpha", ParticleBlend::Alpha)
        .value("Premultiplied", ParticleBlend::Premultiplied);
    OX_REFLECT_ENUM(ParticleRenderMode, "ParticleRenderMode")
        .value("Billboard", ParticleRenderMode::Billboard)
        .value("StretchedBillboard", ParticleRenderMode::StretchedBillboard)
        .value("Mesh", ParticleRenderMode::Mesh);
    OX_REFLECT_ENUM(ParticleCollision, "ParticleCollision")
        .value("None", ParticleCollision::None)
        .value("Bounce", ParticleCollision::Bounce)
        .value("Kill", ParticleCollision::Kill);
    OX_REFLECT_ENUM(ParticleSprite, "ParticleSprite")
        .value("SoftCircle", ParticleSprite::SoftCircle)
        .value("Smoke", ParticleSprite::Smoke)
        .value("Spark", ParticleSprite::Spark);

    OX_REFLECT_TYPE(ParticleBurst, "ParticleBurst")
        .field("time", &ParticleBurst::time, Range{0.0, 1000.0})
        .field("count", &ParticleBurst::count, Range{0.0, 100000.0})
        .field("cycles", &ParticleBurst::cycles, Tooltip{"0 = repeat forever"})
        .field("interval", &ParticleBurst::interval, Range{0.0, 1000.0});
    OX_REFLECT_TYPE(ParticleCurveKey, "ParticleCurveKey")
        .field("time", &ParticleCurveKey::time, Range{0.0, 1.0})
        .field("value", &ParticleCurveKey::value);
    OX_REFLECT_TYPE(ParticleColorKey, "ParticleColorKey")
        .field("time", &ParticleColorKey::time, Range{0.0, 1.0})
        .field("color", &ParticleColorKey::color, Color{true});

    OX_REFLECT_TYPE(ParticleEmitterComponent, "ParticleEmitter")
        .attributes(Category{"Effects"}, Meta{"icon", "particles"})
        .field("enabled", &ParticleEmitterComponent::enabled)
        .field("maxParticles", &ParticleEmitterComponent::maxParticles, Range{1.0, 1048576.0})
        .field("seed", &ParticleEmitterComponent::seed)
        .field("spawnRate", &ParticleEmitterComponent::spawnRate, Range{0.0, 100000.0}, Category{"Spawn"})
        .field("bursts", &ParticleEmitterComponent::bursts, Category{"Spawn"})
        .field("loop", &ParticleEmitterComponent::loop, Category{"Spawn"})
        .field("duration", &ParticleEmitterComponent::duration, Range{0.01, 1000.0}, Category{"Spawn"})
        .field("lifetime", &ParticleEmitterComponent::lifetime, Category{"Spawn"}, Tooltip{"min, max seconds"})
        .field("shape", &ParticleEmitterComponent::shape, Category{"Shape"})
        .field("radius", &ParticleEmitterComponent::radius, Range{0.0, 1000.0}, Category{"Shape"})
        .field("coneAngle", &ParticleEmitterComponent::coneAngle, Range{0.0, 180.0}, Category{"Shape"})
        .field("boxExtents", &ParticleEmitterComponent::boxExtents, Category{"Shape"})
        .field("shapeMesh", &ParticleEmitterComponent::shapeMesh, AssetRef{"Mesh"}, Category{"Shape"})
        .field("emitFromShell", &ParticleEmitterComponent::emitFromShell, Category{"Shape"})
        .field("speed", &ParticleEmitterComponent::speed, Category{"Motion"})
        .field("velocity", &ParticleEmitterComponent::velocity, Category{"Motion"})
        .field("gravity", &ParticleEmitterComponent::gravity, Category{"Motion"})
        .field("gravityScale", &ParticleEmitterComponent::gravityScale, Range{-10.0, 10.0}, Category{"Motion"})
        .field("drag", &ParticleEmitterComponent::drag, Range{0.0, 100.0}, Category{"Motion"})
        .field("turbulence", &ParticleEmitterComponent::turbulence, Range{0.0, 1000.0}, Category{"Motion"})
        .field("turbulenceFrequency", &ParticleEmitterComponent::turbulenceFrequency, Range{0.0, 100.0},
               Category{"Motion"})
        .field("turbulenceSpeed", &ParticleEmitterComponent::turbulenceSpeed, Category{"Motion"})
        .field("size", &ParticleEmitterComponent::size, Category{"Appearance"})
        .field("sizeOverLife", &ParticleEmitterComponent::sizeOverLife, Category{"Appearance"})
        .field("color", &ParticleEmitterComponent::color, Color{true}, Category{"Appearance"})
        .field("colorOverLife", &ParticleEmitterComponent::colorOverLife, Category{"Appearance"})
        .field("emissive", &ParticleEmitterComponent::emissive, Range{0.0, 1000.0}, Category{"Appearance"})
        .field("rotation", &ParticleEmitterComponent::rotation, Category{"Appearance"})
        .field("rotationSpeed", &ParticleEmitterComponent::rotationSpeed, Category{"Appearance"})
        .field("texture", &ParticleEmitterComponent::texture, AssetRef{"Texture"}, Category{"Appearance"})
        .field("sprite", &ParticleEmitterComponent::sprite, Category{"Appearance"})
        .field("atlasColumns", &ParticleEmitterComponent::atlasColumns, Range{1.0, 64.0}, Category{"Appearance"})
        .field("atlasRows", &ParticleEmitterComponent::atlasRows, Range{1.0, 64.0}, Category{"Appearance"})
        .field("flipbookFps", &ParticleEmitterComponent::flipbookFps, Range{0.0, 240.0}, Category{"Appearance"})
        .field("randomStartFrame", &ParticleEmitterComponent::randomStartFrame, Category{"Appearance"})
        .field("blend", &ParticleEmitterComponent::blend, Category{"Rendering"})
        .field("renderMode", &ParticleEmitterComponent::renderMode, Category{"Rendering"})
        .field("stretch", &ParticleEmitterComponent::stretch, Range{0.0, 10.0}, Category{"Rendering"})
        .field("mesh", &ParticleEmitterComponent::mesh, AssetRef{"Mesh"}, Category{"Rendering"})
        .field("lit", &ParticleEmitterComponent::lit, Category{"Rendering"})
        .field("softDistance", &ParticleEmitterComponent::softDistance, Range{0.0, 100.0}, Category{"Rendering"})
        .field("sort", &ParticleEmitterComponent::sort, Category{"Rendering"})
        .field("collision", &ParticleEmitterComponent::collision, Category{"Collision"})
        .field("bounce", &ParticleEmitterComponent::bounce, Range{0.0, 1.0}, Category{"Collision"})
        .field("friction", &ParticleEmitterComponent::friction, Range{0.0, 1.0}, Category{"Collision"});

    OX_REFLECT_TYPE(WaterWave, "WaterWave")
        .field("direction", &WaterWave::direction)
        .field("wavelength", &WaterWave::wavelength, Range{0.01, 10000.0})
        .field("amplitude", &WaterWave::amplitude, Range{0.0, 100.0}, Step{0.01})
        .field("steepness", &WaterWave::steepness, Range{0.0, 1.0})
        .field("phase", &WaterWave::phase)
        .field("speedScale", &WaterWave::speedScale, Range{0.0, 10.0});

    OX_REFLECT_TYPE(WaterSurfaceComponent, "WaterSurface")
        .attributes(Category{"Rendering"}, Meta{"icon", "water"})
        .field("visible", &WaterSurfaceComponent::visible)
        .field("waves", &WaterSurfaceComponent::waves)
        .field("size", &WaterSurfaceComponent::size, Tooltip{"XZ extent in metres (<= 0 = unbounded)"})
        .field("absorption", &WaterSurfaceComponent::absorption, Category{"Optics"}, Tooltip{"Beer-Lambert 1/m"})
        .field("scatterColor", &WaterSurfaceComponent::scatterColor, Color{}, Category{"Optics"})
        .field("refractionStrength", &WaterSurfaceComponent::refractionStrength, Range{0.0, 1.0}, Category{"Optics"})
        .field("roughness", &WaterSurfaceComponent::roughness, Range{0.0, 1.0}, Category{"Optics"})
        .field("normalMap", &WaterSurfaceComponent::normalMap, AssetRef{"Texture"}, Category{"Optics"})
        .field("detailNormalStrength", &WaterSurfaceComponent::detailNormalStrength, Range{0.0, 2.0}, Category{"Optics"})
        .field("detailScale", &WaterSurfaceComponent::detailScale, Range{0.0, 10.0}, Category{"Optics"})
        .field("detailSpeed", &WaterSurfaceComponent::detailSpeed, Range{0.0, 10.0}, Category{"Optics"})
        .field("shoreFoamDistance", &WaterSurfaceComponent::shoreFoamDistance, Range{0.0, 10.0}, Category{"Foam"})
        .field("crestFoam", &WaterSurfaceComponent::crestFoam, Range{0.0, 1.0}, Category{"Foam"})
        .field("foamIntensity", &WaterSurfaceComponent::foamIntensity, Range{0.0, 4.0}, Category{"Foam"})
        .field("causticsIntensity", &WaterSurfaceComponent::causticsIntensity, Range{0.0, 10.0}, Category{"Caustics"})
        .field("causticsScale", &WaterSurfaceComponent::causticsScale, Range{0.0, 10.0}, Category{"Caustics"})
        .field("causticsFalloff", &WaterSurfaceComponent::causticsFalloff, Range{0.0, 10.0}, Category{"Caustics"})
        .field("underwaterColor", &WaterSurfaceComponent::underwaterColor, Color{}, Category{"Underwater"})
        .field("underwaterDensity", &WaterSurfaceComponent::underwaterDensity, Range{0.0, 10.0}, Category{"Underwater"});

    auto& reg = ComponentRegistry::instance();
    reg.add<ParticleEmitterComponent>();
    reg.add<WaterSurfaceComponent>();
    installTranslucencyExtractHook();
}

} // namespace ox::render
