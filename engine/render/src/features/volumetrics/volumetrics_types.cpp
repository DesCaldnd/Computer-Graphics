// Volumetrics: component reflection / registration, snapshot extension and extract hook.

#include <oxwald/core/reflect.hpp>
#include <oxwald/render/features/volumetrics/volumetrics.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/world.hpp>

#include <mutex>

namespace ox::render::volumetrics {

void VolumetricsSnapshot::clear() {
    volumes.clear();
    fog.reset();
    clouds.reset();
    hasWind = false;
    wind = glm::vec3(0.0f);
}

void setWorldWind(RenderSnapshot& snapshot, glm::vec3 metresPerSecond) {
    VolumetricsSnapshot& v = snapshot.extension<VolumetricsSnapshot>();
    v.hasWind = true;
    v.wind = metresPerSecond;
}

bool fogActive(const RenderSnapshot& snapshot) {
    if (snapshot.environment && snapshot.environment->environment.fogEnabled &&
        snapshot.environment->environment.fogDensity > 0.0f) {
        return true;
    }
    const VolumetricsSnapshot* v = snapshot.findExtension<VolumetricsSnapshot>();
    return v && !v->volumes.empty();
}

namespace {

bool activeInHierarchy(const entt::registry& reg, entt::entity e) {
    for (u32 depth = 0; e != entt::null && depth < 1024; ++depth) {
        if (const auto* a = reg.try_get<ActiveComponent>(e); a && !a->active) return false;
        const auto* h = reg.try_get<HierarchyComponent>(e);
        e = h ? h->parent : entt::entity{entt::null};
    }
    return true;
}

} // namespace

void extractVolumetrics(const World& world, RenderSnapshot& out) {
    const entt::registry& reg = world.registry();
    VolumetricsSnapshot* ext = nullptr;
    auto get = [&]() -> VolumetricsSnapshot& {
        if (!ext) ext = &out.extension<VolumetricsSnapshot>();
        return *ext;
    };
    for (auto [e, fv, wt] : reg.view<const FogVolumeComponent, const WorldTransformComponent>().each()) {
        if (!activeInHierarchy(reg, e) || fv.density <= 0.0f) continue;
        get().volumes.push_back({fv, wt.matrix, encodeEntityId(u32(entt::to_integral(e)))});
    }
    for (auto [e, f] : reg.view<const VolumetricFogComponent>().each()) {
        if (!activeInHierarchy(reg, e)) continue;
        get().fog = f;
        break;
    }
    for (auto [e, c] : reg.view<const CloudLayerComponent>().each()) {
        if (!activeInHierarchy(reg, e)) continue;
        get().clouds = c;
        break;
    }
}

void registerVolumetricsTypes() {
    using attr::Category;
    using attr::Color;
    using attr::Meta;
    using attr::Range;
    using attr::Tooltip;

    OX_REFLECT_ENUM(FogVolumeShape, "FogVolumeShape")
        .value("Box", FogVolumeShape::Box)
        .value("Sphere", FogVolumeShape::Sphere)
        .value("Ellipsoid", FogVolumeShape::Ellipsoid);

    OX_REFLECT_TYPE(FogVolumeComponent, "FogVolume")
        .attributes(Category{"Rendering"}, Meta{"icon", "cloud"})
        .field("shape", &FogVolumeComponent::shape)
        .field("extents", &FogVolumeComponent::extents, Range{0.0, 100000.0}, Tooltip{"Local half size (m)"})
        .field("density", &FogVolumeComponent::density, Range{0.0, 10.0}, Tooltip{"Extinction (1/m)"})
        .field("albedo", &FogVolumeComponent::albedo, Color{})
        .field("emission", &FogVolumeComponent::emission, Color{true})
        .field("falloff", &FogVolumeComponent::falloff, Range{0.0, 1.0})
        .field("anisotropy", &FogVolumeComponent::anisotropy, Range{-0.95, 0.95})
        .field("noiseIntensity", &FogVolumeComponent::noiseIntensity, Range{0.0, 1.0}, Category{"Noise"})
        .field("noiseScale", &FogVolumeComponent::noiseScale, Range{0.1, 10000.0}, Category{"Noise"})
        .field("noiseVelocity", &FogVolumeComponent::noiseVelocity, Category{"Noise"})
        .field("windInfluence", &FogVolumeComponent::windInfluence, Range{0.0, 10.0}, Category{"Noise"});

    OX_REFLECT_TYPE(VolumetricFogComponent, "VolumetricFog")
        .attributes(Category{"Rendering"}, Meta{"icon", "cloud"})
        .field("anisotropy", &VolumetricFogComponent::anisotropy, Range{-0.95, 0.95})
        .field("ambientIntensity", &VolumetricFogComponent::ambientIntensity, Range{0.0, 100.0})
        .field("directionalIntensity", &VolumetricFogComponent::directionalIntensity, Range{0.0, 100.0})
        .field("localLightIntensity", &VolumetricFogComponent::localLightIntensity, Range{0.0, 100.0})
        .field("emission", &VolumetricFogComponent::emission, Color{true})
        .field("distance", &VolumetricFogComponent::distance, Range{0.0, 10000.0},
               Tooltip{"Froxel grid distance (m), 0 = r.VolumetricFog.Distance"});

    OX_REFLECT_TYPE(CloudLayerComponent, "CloudLayer")
        .attributes(Category{"Rendering"}, Meta{"icon", "cloud"})
        .field("altitude", &CloudLayerComponent::altitude, Range{0.0, 20000.0})
        .field("thickness", &CloudLayerComponent::thickness, Range{10.0, 20000.0})
        .field("coverage", &CloudLayerComponent::coverage, Range{0.0, 1.0})
        .field("cloudType", &CloudLayerComponent::cloudType, Range{0.0, 1.0})
        .field("density", &CloudLayerComponent::density, Range{0.0, 10.0})
        .field("albedo", &CloudLayerComponent::albedo, Color{})
        .field("windDirection", &CloudLayerComponent::windDirection, Category{"Wind"})
        .field("windSpeed", &CloudLayerComponent::windSpeed, Range{0.0, 200.0}, Category{"Wind"})
        .field("weatherScale", &CloudLayerComponent::weatherScale, Range{100.0, 1000000.0}, Category{"Shape"})
        .field("shapeScale", &CloudLayerComponent::shapeScale, Range{10.0, 100000.0}, Category{"Shape"})
        .field("detailScale", &CloudLayerComponent::detailScale, Range{1.0, 10000.0}, Category{"Shape"})
        .field("detailStrength", &CloudLayerComponent::detailStrength, Range{0.0, 1.0}, Category{"Shape"})
        .field("weatherOffset", &CloudLayerComponent::weatherOffset, Category{"Shape"})
        .field("ambientIntensity", &CloudLayerComponent::ambientIntensity, Range{0.0, 10.0}, Category{"Lighting"})
        .field("sunIntensity", &CloudLayerComponent::sunIntensity, Range{0.0, 10.0}, Category{"Lighting"})
        .field("forwardScattering", &CloudLayerComponent::forwardScattering, Range{0.0, 0.95}, Category{"Lighting"})
        .field("shadowStrength", &CloudLayerComponent::shadowStrength, Range{0.0, 1.0}, Category{"Lighting"});

    auto& reg = ComponentRegistry::instance();
    reg.add<FogVolumeComponent>();
    reg.add<VolumetricFogComponent>();
    reg.add<CloudLayerComponent>();

    static std::once_flag once;
    std::call_once(once, [] { addExtractHook(&extractVolumetrics); });
}

} // namespace ox::render::volumetrics
