#include <oxwald/render/components/postprocess.hpp>
#include <oxwald/render/components/reflections.hpp>
#include <oxwald/render/components/translucency.hpp>
#include <oxwald/render/features/volumetrics/volumetrics.hpp>
#include <oxwald/render/components/world.hpp>
#include <oxwald/render/register_types.hpp>

namespace ox::render {

// Each feature area adds one line calling its register...Types() (all idempotent).
void registerRenderTypes() {
    registerReflectionTypes();
    volumetrics::registerVolumetricsTypes();
    registerTranslucencyTypes();
    registerWorldSkinningTypes();
    registerPostProcessTypes();
}

} // namespace ox::render
