// Components (reflection + registry) and the extract hook of the reflections-ao area.
#include <oxwald/core/reflect.hpp>
#include <oxwald/render/components/reflections.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/components.hpp>
#include <oxwald/scene/world.hpp>

#include <mutex>

namespace ox::render {

namespace {

bool activeInHierarchy(const entt::registry& reg, entt::entity e) {
    for (u32 depth = 0; e != entt::null && depth < 1024; ++depth) {
        if (const auto* a = reg.try_get<ActiveComponent>(e); a && !a->active) return false;
        const auto* h = reg.try_get<HierarchyComponent>(e);
        e = h ? h->parent : entt::entity{entt::null};
    }
    return true;
}

std::once_flag g_hookOnce;

} // namespace

namespace reflections {

void extractReflections(const World& world, RenderSnapshot& out) {
    const entt::registry& reg = world.registry();
    ReflectionSnapshot* data = nullptr;
    auto get = [&]() -> ReflectionSnapshot& {
        if (!data) data = &out.extension<ReflectionSnapshot>();
        return *data;
    };
    auto uuidOf = [&](entt::entity e) {
        const auto* id = reg.try_get<IdComponent>(e);
        return id ? id->id : Uuid{};
    };
    for (auto [e, probe, wt] : reg.view<const ReflectionProbeComponent, const WorldTransformComponent>().each()) {
        if (!probe.enabled || !activeInHierarchy(reg, e)) continue;
        get().probes.push_back({encodeEntityId(u32(entt::to_integral(e))), uuidOf(e), wt.matrix, probe});
    }
    for (auto [e, planar, wt] : reg.view<const PlanarReflectorComponent, const WorldTransformComponent>().each()) {
        if (!planar.enabled || !activeInHierarchy(reg, e)) continue;
        get().planars.push_back({encodeEntityId(u32(entt::to_integral(e))), wt.matrix, planar});
    }
    for (auto [e, vol, wt] : reg.view<const IrradianceVolumeComponent, const WorldTransformComponent>().each()) {
        if (!vol.enabled || !activeInHierarchy(reg, e)) continue;
        get().volumes.push_back({encodeEntityId(u32(entt::to_integral(e))), uuidOf(e), wt.matrix, vol});
    }
}

} // namespace reflections

void registerReflectionTypes() {
    using attr::Category;
    using attr::Meta;
    using attr::Range;
    using attr::Tooltip;

    OX_REFLECT_ENUM(ReflectionProbeUpdate, "ReflectionProbeUpdate")
        .value("Baked", ReflectionProbeUpdate::Baked)
        .value("OnEnable", ReflectionProbeUpdate::OnEnable)
        .value("Realtime", ReflectionProbeUpdate::Realtime);

    OX_REFLECT_TYPE(ReflectionProbeComponent, "ReflectionProbe")
        .attributes(Category{"Rendering"}, Meta{"icon", "globe"})
        .field("enabled", &ReflectionProbeComponent::enabled)
        .field("extents", &ReflectionProbeComponent::extents, Tooltip{"Half size of the influence / projection box (m)"})
        .field("blendDistance", &ReflectionProbeComponent::blendDistance, Range{0.0, 100.0})
        .field("boxProjection", &ReflectionProbeComponent::boxProjection)
        .field("captureOffset", &ReflectionProbeComponent::captureOffset)
        .field("resolution", &ReflectionProbeComponent::resolution, Range{16.0, 1024.0})
        .field("update", &ReflectionProbeComponent::update)
        .field("priority", &ReflectionProbeComponent::priority, Range{-1000.0, 1000.0})
        .field("intensity", &ReflectionProbeComponent::intensity, Range{0.0, 16.0})
        .field("nearPlane", &ReflectionProbeComponent::nearPlane, Range{0.001, 10.0})
        .field("farPlane", &ReflectionProbeComponent::farPlane, Range{1.0, 100000.0});

    OX_REFLECT_TYPE(PlanarReflectorComponent, "PlanarReflector")
        .attributes(Category{"Rendering"}, Meta{"icon", "square"})
        .field("enabled", &PlanarReflectorComponent::enabled)
        .field("size", &PlanarReflectorComponent::size, Tooltip{"Half extents in local XZ (0 = infinite plane)"})
        .field("resolutionScale", &PlanarReflectorComponent::resolutionScale, Range{0.1, 2.0})
        .field("clipOffset", &PlanarReflectorComponent::clipOffset, Range{-1.0, 1.0})
        .field("maxDistance", &PlanarReflectorComponent::maxDistance, Range{1.0, 100000.0})
        .field("maxRoughness", &PlanarReflectorComponent::maxRoughness, Range{0.0, 1.0})
        .field("distortion", &PlanarReflectorComponent::distortion, Range{0.0, 1.0})
        .field("intensity", &PlanarReflectorComponent::intensity, Range{0.0, 16.0})
        .field("priority", &PlanarReflectorComponent::priority, Range{-1000.0, 1000.0});

    OX_REFLECT_TYPE(IrradianceVolumeComponent, "IrradianceVolume")
        .attributes(Category{"Rendering"}, Meta{"icon", "grid"})
        .field("enabled", &IrradianceVolumeComponent::enabled)
        .field("extents", &IrradianceVolumeComponent::extents, Tooltip{"Half size of the probe grid (m)"})
        .field("probeCount", &IrradianceVolumeComponent::probeCount, Range{2.0, 64.0})
        .field("blendDistance", &IrradianceVolumeComponent::blendDistance, Range{0.0, 100.0})
        .field("intensity", &IrradianceVolumeComponent::intensity, Range{0.0, 16.0})
        .field("normalBias", &IrradianceVolumeComponent::normalBias, Range{0.0, 2.0})
        .field("viewBias", &IrradianceVolumeComponent::viewBias, Range{0.0, 2.0})
        .field("captureResolution", &IrradianceVolumeComponent::captureResolution, Range{8.0, 128.0})
        .field("priority", &IrradianceVolumeComponent::priority, Range{-1000.0, 1000.0});

    auto& reg = ComponentRegistry::instance();
    reg.add<ReflectionProbeComponent>();
    reg.add<PlanarReflectorComponent>();
    reg.add<IrradianceVolumeComponent>();
    std::call_once(g_hookOnce, [] { addExtractHook(&reflections::extractReflections); });
}

} // namespace ox::render
