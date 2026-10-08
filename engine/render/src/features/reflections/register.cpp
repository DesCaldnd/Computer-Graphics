// Registration of the reflections-ao area (called from Renderer::registerBuiltinFeatures).
#include "reflection_internal.hpp"

namespace ox::render {

void registerReflectionFeatures(FeatureRegistry& registry) {
    registerReflectionTypes();
    registerReflectionCVars();
    registry.add(reflections::makeAmbientOcclusionFeature());
    registry.add(reflections::makeReflectionsFeature());
    registry.add(reflections::makeIrradianceVolumesFeature());
}

} // namespace ox::render
