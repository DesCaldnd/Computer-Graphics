#include <oxwald/core/cvar.hpp>
#include <oxwald/render/neural/neural.hpp>

namespace ox::render::neural {

bool neuralDenoiserSelected() {
    // r.Denoiser is declared by the postprocess area (pp_cvars.cpp); value 3 = Neural.
    static CVar<int>* denoiser = CVarRegistry::instance().findAs<int>("r.Denoiser");
    if (!denoiser) denoiser = CVarRegistry::instance().findAs<int>("r.Denoiser");
    return denoiser && denoiser->get() == 3;
}

} // namespace ox::render::neural
