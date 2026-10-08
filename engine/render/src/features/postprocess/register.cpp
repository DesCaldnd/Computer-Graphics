#include "pp_common.hpp"

namespace ox::render {

void registerPostProcessFeatures(FeatureRegistry& f) {
    pp::registerCVars();
    registerPostProcessTypes();
    // Registration order = prepareView order (auto exposure first; exactly one AA/upscaler path is enabled).
    f.add(pp::makeAutoExposureFeature());
    f.add(pp::makeTaaFeature());
    f.add(pp::makeTaauFeature());
    f.add(pp::makeDlssFeature());
    f.add(pp::makeFsr1Feature());
    f.add(pp::makeDepthOfFieldFeature());
    f.add(pp::makeMotionBlurFeature());
    f.add(pp::makeBloomFeature());
    f.add(pp::makeSharpenFeature());
    f.add(pp::makeCompositeFeature());
    f.add(pp::makeLdrPostFeature());
}

} // namespace ox::render
