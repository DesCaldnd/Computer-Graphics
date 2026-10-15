#include <oxwald/render/neural/neural.hpp>

namespace ox::render::neural {

void registerNeuralTextureFeature(Renderer& renderer); // neural_textures.cpp

void registerNeuralFeatures(Renderer& renderer) {
    registerNeuralTextureFeature(renderer);
    // The neural denoiser has no feature of its own: rt::SvgfDenoiser switches its À-trous kernel (denoiser.cpp).
}

} // namespace ox::render::neural
