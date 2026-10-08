// Registration of the ray tracing area (renderer.cpp marker line "[feature-area: raytracing]").
#include "rt_internal.hpp"

namespace ox::render {

void registerRayTracingFeatures(FeatureRegistry& registry) {
    rt::registerRayTracingCVars();
    auto shared = std::make_shared<rt::RtShared>();
    registry.add(rt::makeRayTracingSceneFeature(shared));
    registry.add(rt::makeShadowsRtFeature(shared));
    registry.add(rt::makeAmbientOcclusionRtFeature(shared));
    registry.add(rt::makeGlobalIlluminationRtFeature(shared));
    registry.add(rt::makeReflectionsRtFeature(shared));
    registry.add(rt::makeVolumetricsRtFeature(shared));
    registry.add(rt::makeTranslucencyRtFeature(shared));
    registry.add(rt::makePathTracerFeature(shared));
}

} // namespace ox::render
