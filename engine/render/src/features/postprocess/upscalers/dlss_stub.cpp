// DLSS backend for builds without the NGX runtime (macOS, OX_ENABLE_DLSS=OFF, SDK libraries missing).
#include "dlss_backend.hpp"

namespace ox::render::dlss {

bool compiled() { return false; }

std::string buildUnavailableReason() {
#if defined(__APPLE__)
    return "NVIDIA DLSS requires an NVIDIA RTX GPU on Windows or Linux (not available on macOS / MoltenVK)";
#elif defined(OX_DLSS_DISABLED_BY_OPTION)
    return "this build was configured with OX_ENABLE_DLSS=OFF";
#else
    return "this build has no NVIDIA DLSS runtime (the nvidia-dlss port provides it on Windows/Linux x64)";
#endif
}

Status probe(rhi::Device&) { return {false, buildUnavailableReason()}; }
bool knownUnavailable() { return true; }
void requiredExtensions(std::vector<std::string>&, std::vector<std::string>&) {}
OptimalSettings optimalSettings(rhi::Device&, u32, u32, UpscalerQuality) { return {}; }
bool evaluate(rhi::Device&, rhi::CommandList&, ViewFeature&, const EvaluateParams&) { return false; }
void release(ViewFeature& feature) { feature = {}; }
void shutdown(rhi::Device&) {}

} // namespace ox::render::dlss
