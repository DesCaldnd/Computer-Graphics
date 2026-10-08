// Общий помощник примеров главы 26: регистрирует все cvar'ы рендерера без GPU.
#pragma once

#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/gpu_driven/gpu_driven.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/features/raytracing/raytracing.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>
#include <oxwald/render/features/translucency/translucency.hpp>
#include <oxwald/render/features/volumetrics/volumetrics.hpp>
#include <oxwald/render/features/world/world_skinning.hpp>
#include <oxwald/render/render.hpp>

namespace guide {

// Renderer::create() делает это сам. Без устройства (лаунчер, меню настроек до старта рендерера, тесты) cvar'ы
// фич регистрируются, когда в программу попадают их единицы трансляции: вызов register*Features() это гарантирует.
inline void registerAllRenderCVars() {
    static ox::render::FeatureRegistry features; // фичи не нужны — только их cvar'ы
    static const bool once = [] {
        ox::render::registerRenderCVars();
        ox::render::registerPostProcessFeatures(features);
        ox::render::registerReflectionFeatures(features);
        ox::render::registerTranslucencyFeatures(features);
        ox::render::volumetrics::registerVolumetricsFeatures(features);
        ox::render::registerRayTracingFeatures(features);
        ox::render::registerWorldSkinningFeatures(features);
        ox::render::registerGpuDrivenFeatures(features);
        return true;
    }();
    (void)once;
}

} // namespace guide
