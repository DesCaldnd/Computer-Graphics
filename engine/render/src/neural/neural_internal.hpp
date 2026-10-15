#pragma once

// Shared helpers of render/neural (latent grid textures).
#include <oxwald/assets/neural/ntc.hpp>
#include <oxwald/rhi/handles.hpp>
#include <oxwald/rhi/vulkan.hpp>

namespace ox::rhi {
class Device;
}

namespace ox::render::neural::detail {

// GPU format of latent grids: Q8 -> RGBA8, Q4 -> B4G4R4A4 (2 bytes per 4 features) when sampleable with linear
// filtering, else RGBA8 (values expanded).
[[nodiscard]] VkFormat latentFormat(rhi::Device& device, assets::NtcLatentFormat format);
// 2D array texture (layers = features / 4) with the grid's values, uploaded asynchronously (transfer queue) or
// synchronously; final state sampled. `bytes` receives the uploaded size.
[[nodiscard]] rhi::TextureHandle uploadLatentGrid(rhi::Device& device, const assets::NeuralTextureSet& set,
                                                  const assets::NtcLatentGrid& grid, bool async, u64* bytes = nullptr);

} // namespace ox::render::neural::detail
