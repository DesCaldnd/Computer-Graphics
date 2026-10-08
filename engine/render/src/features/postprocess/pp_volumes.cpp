// PostProcessVolumeComponent: reflection, extraction into the snapshot and per-view blending.
#include "pp_common.hpp"

#include <oxwald/core/reflect.hpp>
#include <oxwald/render/render_settings.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/components.hpp>
#include <oxwald/scene/world.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ox::render {

namespace {

struct PostProcessVolumesSnapshot final : ISnapshotExtension {
    std::vector<PostProcessVolumeSnapshot> volumes;
    void clear() override { volumes.clear(); }
};

bool activeInHierarchy(const entt::registry& reg, entt::entity e) {
    for (u32 depth = 0; e != entt::null && depth < 1024; ++depth) {
        if (const auto* a = reg.try_get<ActiveComponent>(e); a && !a->active) return false;
        const auto* h = reg.try_get<HierarchyComponent>(e);
        e = h ? h->parent : entt::entity{entt::null};
    }
    return true;
}

void extractVolumes(const World& world, RenderSnapshot& out) {
    const entt::registry& reg = world.registry();
    auto view = reg.view<const PostProcessVolumeComponent, const WorldTransformComponent>();
    if (view.begin() == view.end()) return;
    auto& ext = out.extension<PostProcessVolumesSnapshot>();
    for (auto [e, v, wt] : view.each()) {
        if (!v.enabled || v.blendWeight <= 0.0f || !activeInHierarchy(reg, e)) continue;
        PostProcessVolumeSnapshot s;
        s.settings = v.settings;
        s.unbound = v.unbound;
        s.blendRadius = std::max(v.blendRadius, 0.0f);
        s.blendWeight = std::clamp(v.blendWeight, 0.0f, 1.0f);
        s.priority = v.priority;
        glm::vec3 t, scale;
        glm::quat r;
        decompose(wt.matrix, t, r, scale);
        s.extents = glm::abs(v.extents * scale);
        s.worldToLocal = glm::inverse(glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r));
        ext.volumes.push_back(s);
    }
}

template <class T>
T lerpT(const T& a, const T& b, f32 w) {
    return a + (b - a) * w;
}

} // namespace

void registerPostProcessTypes() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    using namespace attr;
    OX_REFLECT_TYPE(PostProcessSettings, "PostProcessSettings")
        .field("overrideExposure", &PostProcessSettings::overrideExposure, Category{"Exposure"})
        .field("autoExposure", &PostProcessSettings::autoExposure, Category{"Exposure"},
               Tooltip{"Histogram eye adaptation instead of the camera's EV100"})
        .field("exposureCompensation", &PostProcessSettings::exposureCompensation, Category{"Exposure"},
               Range{-10.0, 10.0}, Tooltip{"EV; positive = brighter"})
        .field("minEV100", &PostProcessSettings::minEV100, Category{"Exposure"}, Range{-10.0, 24.0})
        .field("maxEV100", &PostProcessSettings::maxEV100, Category{"Exposure"}, Range{-10.0, 24.0})
        .field("adaptationSpeedUp", &PostProcessSettings::adaptationSpeedUp, Category{"Exposure"}, Range{0.01, 100.0})
        .field("adaptationSpeedDown", &PostProcessSettings::adaptationSpeedDown, Category{"Exposure"},
               Range{0.01, 100.0})
        .field("histogramLowPercent", &PostProcessSettings::histogramLowPercent, Category{"Exposure"},
               Range{0.0, 100.0})
        .field("histogramHighPercent", &PostProcessSettings::histogramHighPercent, Category{"Exposure"},
               Range{0.0, 100.0})
        .field("overrideBloom", &PostProcessSettings::overrideBloom, Category{"Bloom"})
        .field("bloomIntensity", &PostProcessSettings::bloomIntensity, Category{"Bloom"}, Range{0.0, 1.0}, Step{0.005})
        .field("bloomDirtTexture", &PostProcessSettings::bloomDirtTexture, Category{"Bloom"}, AssetRef{"Texture"})
        .field("bloomDirtIntensity", &PostProcessSettings::bloomDirtIntensity, Category{"Bloom"}, Range{0.0, 10.0})
        .field("overrideDepthOfField", &PostProcessSettings::overrideDepthOfField, Category{"Depth of Field"})
        .field("focusDistance", &PostProcessSettings::focusDistance, Category{"Depth of Field"}, Range{0.0, 100000.0},
               Tooltip{"Metres; 0 disables depth of field"})
        .field("aperture", &PostProcessSettings::aperture, Category{"Depth of Field"}, Range{0.0, 64.0},
               Tooltip{"f-number; 0 = camera aperture"})
        .field("focalLength", &PostProcessSettings::focalLength, Category{"Depth of Field"}, Range{0.0, 2000.0},
               Tooltip{"mm; 0 = from the field of view on a 24 mm sensor"})
        .field("maxBokehSize", &PostProcessSettings::maxBokehSize, Category{"Depth of Field"}, Range{0.0, 5.0},
               Tooltip{"Largest blur radius in percent of the view width"})
        .field("overrideMotionBlur", &PostProcessSettings::overrideMotionBlur, Category{"Motion Blur"})
        .field("motionBlurAmount", &PostProcessSettings::motionBlurAmount, Category{"Motion Blur"}, Range{0.0, 1.0},
               Tooltip{"Shutter fraction of the frame (0.5 = 180°)"})
        .field("motionBlurMax", &PostProcessSettings::motionBlurMax, Category{"Motion Blur"}, Range{0.0, 20.0},
               Tooltip{"Longest blur in percent of the view width"})
        .field("overrideWhiteBalance", &PostProcessSettings::overrideWhiteBalance, Category{"Color Grading"})
        .field("temperature", &PostProcessSettings::temperature, Category{"Color Grading"}, Range{1500.0, 15000.0})
        .field("tint", &PostProcessSettings::tint, Category{"Color Grading"}, Range{-1.0, 1.0})
        .field("overrideGrading", &PostProcessSettings::overrideGrading, Category{"Color Grading"})
        .field("saturation", &PostProcessSettings::saturation, Category{"Color Grading"}, Range{0.0, 2.0})
        .field("contrast", &PostProcessSettings::contrast, Category{"Color Grading"}, Range{0.0, 2.0})
        .field("lift", &PostProcessSettings::lift, Category{"Color Grading"})
        .field("gamma", &PostProcessSettings::gamma, Category{"Color Grading"})
        .field("gain", &PostProcessSettings::gain, Category{"Color Grading"})
        .field("overrideLut", &PostProcessSettings::overrideLut, Category{"Color Grading"})
        .field("lutTexture", &PostProcessSettings::lutTexture, Category{"Color Grading"}, AssetRef{"Texture"},
               Tooltip{"N²×N strip LUT (e.g. 1024×32), display referred"})
        .field("lutIntensity", &PostProcessSettings::lutIntensity, Category{"Color Grading"}, Range{0.0, 1.0})
        .field("overrideLens", &PostProcessSettings::overrideLens, Category{"Lens"})
        .field("vignetteIntensity", &PostProcessSettings::vignetteIntensity, Category{"Lens"}, Range{0.0, 1.0})
        .field("chromaticAberration", &PostProcessSettings::chromaticAberration, Category{"Lens"}, Range{0.0, 1.0})
        .field("filmGrainIntensity", &PostProcessSettings::filmGrainIntensity, Category{"Lens"}, Range{0.0, 1.0})
        .field("sharpen", &PostProcessSettings::sharpen, Category{"Lens"}, Range{0.0, 1.0});
    OX_REFLECT_TYPE(PostProcessVolumeComponent, "PostProcessVolume")
        .attributes(Category{"Rendering"}, Meta{"icon", "postprocess"})
        .field("enabled", &PostProcessVolumeComponent::enabled)
        .field("unbound", &PostProcessVolumeComponent::unbound, Tooltip{"Affects the whole world (else a box)"})
        .field("extents", &PostProcessVolumeComponent::extents, Tooltip{"Box half extents (local space)"})
        .field("blendRadius", &PostProcessVolumeComponent::blendRadius, Range{0.0, 10000.0},
               Tooltip{"Metres outside the box over which the volume fades out"})
        .field("blendWeight", &PostProcessVolumeComponent::blendWeight, Range{0.0, 1.0})
        .field("priority", &PostProcessVolumeComponent::priority, Tooltip{"Higher priority volumes win"})
        .field("settings", &PostProcessVolumeComponent::settings);
    ComponentRegistry::instance().add<PostProcessVolumeComponent>();
    addExtractHook(&extractVolumes);
}

f32 postProcessVolumeWeight(const PostProcessVolumeSnapshot& v, const glm::vec3& position) {
    if (v.unbound) return v.blendWeight;
    const glm::vec3 local = glm::vec3(v.worldToLocal * glm::vec4(position, 1.0f));
    const glm::vec3 outside = glm::max(glm::abs(local) - v.extents, glm::vec3(0.0f));
    const f32 d = glm::length(outside);
    if (d <= 0.0f) return v.blendWeight;
    if (v.blendRadius <= 0.0f) return 0.0f;
    return v.blendWeight * std::max(0.0f, 1.0f - d / v.blendRadius);
}

PostProcessSettings postProcessDefaults(const RenderSettings& rs) {
    PostProcessSettings s;
    s.autoExposure = pp::cvAutoExposure.get();
    s.minEV100 = pp::cvExposureMinEV.get();
    s.maxEV100 = pp::cvExposureMaxEV.get();
    s.adaptationSpeedUp = pp::cvExposureSpeedUp.get();
    s.adaptationSpeedDown = pp::cvExposureSpeedDown.get();
    s.bloomIntensity = pp::cvBloomIntensity.get();
    s.sharpen = pp::cvSharpen.get();
    (void)rs;
    return s;
}

PostProcessSettings blendPostProcessVolumes(const PostProcessSettings& base,
                                            std::span<const PostProcessVolumeSnapshot> volumes,
                                            const glm::vec3& position) {
    PostProcessSettings s = base;
    std::vector<std::pair<const PostProcessVolumeSnapshot*, f32>> active;
    for (const PostProcessVolumeSnapshot& v : volumes) {
        const f32 w = postProcessVolumeWeight(v, position);
        if (w > 0.0f) active.push_back({&v, w});
    }
    std::stable_sort(active.begin(), active.end(),
                     [](const auto& a, const auto& b) { return a.first->priority < b.first->priority; });
    for (const auto& [vp, w] : active) {
        const PostProcessSettings& o = vp->settings;
        const bool switchOver = w >= 0.5f; // discrete values (flags, textures) switch at half weight
        if (o.overrideExposure) {
            if (switchOver) s.autoExposure = o.autoExposure;
            s.exposureCompensation = lerpT(s.exposureCompensation, o.exposureCompensation, w);
            s.minEV100 = lerpT(s.minEV100, o.minEV100, w);
            s.maxEV100 = lerpT(s.maxEV100, o.maxEV100, w);
            s.adaptationSpeedUp = lerpT(s.adaptationSpeedUp, o.adaptationSpeedUp, w);
            s.adaptationSpeedDown = lerpT(s.adaptationSpeedDown, o.adaptationSpeedDown, w);
            s.histogramLowPercent = lerpT(s.histogramLowPercent, o.histogramLowPercent, w);
            s.histogramHighPercent = lerpT(s.histogramHighPercent, o.histogramHighPercent, w);
        }
        if (o.overrideBloom) {
            s.bloomIntensity = lerpT(s.bloomIntensity, o.bloomIntensity, w);
            if (o.bloomDirtTexture.isValid() && (!s.bloomDirtTexture.isValid() || switchOver)) {
                // Fade a newly introduced dirt mask in with the weight.
                if (!s.bloomDirtTexture.isValid()) s.bloomDirtIntensity = 0.0f;
                s.bloomDirtTexture = o.bloomDirtTexture;
            }
            s.bloomDirtIntensity = lerpT(s.bloomDirtIntensity, o.bloomDirtIntensity, w);
        }
        if (o.overrideDepthOfField) {
            // Focus distance 0 means "off": fade the aperture open instead of sweeping the focus plane.
            if (s.focusDistance <= 0.0f) s.focusDistance = o.focusDistance;
            else if (o.focusDistance > 0.0f) s.focusDistance = lerpT(s.focusDistance, o.focusDistance, w);
            else if (switchOver) s.focusDistance = 0.0f;
            s.aperture = (s.aperture > 0.0f && o.aperture > 0.0f) ? lerpT(s.aperture, o.aperture, w)
                         : switchOver || s.aperture <= 0.0f   ? o.aperture
                                                              : s.aperture;
            s.focalLength = (s.focalLength > 0.0f && o.focalLength > 0.0f) ? lerpT(s.focalLength, o.focalLength, w)
                            : switchOver || s.focalLength <= 0.0f        ? o.focalLength
                                                                         : s.focalLength;
            s.maxBokehSize = lerpT(s.maxBokehSize, o.maxBokehSize, w);
        }
        if (o.overrideMotionBlur) {
            s.motionBlurAmount = lerpT(s.motionBlurAmount, o.motionBlurAmount, w);
            s.motionBlurMax = lerpT(s.motionBlurMax, o.motionBlurMax, w);
        }
        if (o.overrideWhiteBalance) {
            s.temperature = lerpT(s.temperature, o.temperature, w);
            s.tint = lerpT(s.tint, o.tint, w);
        }
        if (o.overrideGrading) {
            s.saturation = lerpT(s.saturation, o.saturation, w);
            s.contrast = lerpT(s.contrast, o.contrast, w);
            s.lift = lerpT(s.lift, o.lift, w);
            s.gamma = lerpT(s.gamma, o.gamma, w);
            s.gain = lerpT(s.gain, o.gain, w);
        }
        if (o.overrideLut && o.lutTexture.isValid()) {
            if (!s.lutTexture.isValid() || s.lutTexture == o.lutTexture) {
                const f32 from = s.lutTexture.isValid() ? s.lutIntensity : 0.0f;
                s.lutTexture = o.lutTexture;
                s.lutIntensity = lerpT(from, o.lutIntensity, w);
            } else if (switchOver) {
                s.lutTexture = o.lutTexture;
                s.lutIntensity = o.lutIntensity * w;
            }
        }
        if (o.overrideLens) {
            s.vignetteIntensity = lerpT(s.vignetteIntensity, o.vignetteIntensity, w);
            s.chromaticAberration = lerpT(s.chromaticAberration, o.chromaticAberration, w);
            s.filmGrainIntensity = lerpT(s.filmGrainIntensity, o.filmGrainIntensity, w);
            s.sharpen = lerpT(s.sharpen, o.sharpen, w);
        }
    }
    return s;
}

PostProcessSettings resolvePostProcessSettings(const RenderSnapshot& snapshot, const RenderSettings& settings,
                                               const glm::vec3& position) {
    const PostProcessSettings base = postProcessDefaults(settings);
    const auto* vols = snapshot.findExtension<PostProcessVolumesSnapshot>();
    if (!vols || vols->volumes.empty()) return base;
    return blendPostProcessVolumes(base, vols->volumes, position);
}

namespace pp {

PostProcessSettings viewSettings(FeatureContext& ctx) {
    return resolvePostProcessSettings(ctx.snapshot(), ctx.settings(), ctx.view().camera().position());
}

f32 viewAperture(FeatureContext& ctx) {
    const RenderSnapshot& snap = ctx.snapshot();
    const glm::mat4& w = ctx.view().camera().world;
    for (const SnapshotCamera& c : snap.cameras) {
        bool same = true;
        for (int i = 0; i < 4 && same; ++i) same = glm::all(glm::epsilonEqual(c.world[i], w[i], 1e-3f));
        if (same) return c.camera.aperture;
    }
    if (const i32 p = snap.primaryCamera(); p >= 0) return snap.cameras[usize(p)].camera.aperture;
    return 4.0f;
}

} // namespace pp

} // namespace ox::render
