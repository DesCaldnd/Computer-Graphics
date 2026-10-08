#include "translucency_internal.hpp"

namespace ox::render {

std::unique_ptr<IRenderFeature> makeTranslucencyFeature();
std::unique_ptr<IRenderFeature> makeWaterFeature();
std::unique_ptr<IRenderFeature> makeUnderwaterFeature();
std::unique_ptr<IRenderFeature> makeParticlesFeature();
void installTranslucencyExtractHook();

void registerTranslucencyFeatures(FeatureRegistry& registry) {
    registerTranslucencyTypes();
    registry.add(makeTranslucencyFeature());
    registry.add(makeWaterFeature());
    registry.add(makeUnderwaterFeature());
    registry.add(makeParticlesFeature());
}

} // namespace ox::render
