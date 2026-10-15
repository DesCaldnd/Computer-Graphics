#include "neural_internal.hpp"

#include <oxwald/rhi/device.hpp>

#include <cstring>
#include <vector>

namespace ox::render::neural::detail {

VkFormat latentFormat(rhi::Device& device, assets::NtcLatentFormat format) {
    if (format == assets::NtcLatentFormat::Q8) return VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device.vkPhysicalDevice(), VK_FORMAT_B4G4R4A4_UNORM_PACK16, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (props.optimalTilingFeatures & need) == need ? VK_FORMAT_B4G4R4A4_UNORM_PACK16 : VK_FORMAT_R8G8B8A8_UNORM;
}

rhi::TextureHandle uploadLatentGrid(rhi::Device& device, const assets::NeuralTextureSet& set,
                                    const assets::NtcLatentGrid& g, bool async, u64* bytes) {
    const VkFormat fmt = latentFormat(device, set.latentFormat);
    rhi::TextureDesc d;
    d.name = "ntc.latents";
    d.format = fmt;
    d.width = g.width;
    d.height = g.height;
    d.arrayLayers = g.layers();
    d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    const usize texels = usize(g.width) * g.height * g.layers();
    std::vector<u8> data;
    if (fmt == VK_FORMAT_B4G4R4A4_UNORM_PACK16) {
        // Sampled .rgba = (bits 4-7, 8-11, 12-15, 0-3): features 0..3 of the layer.
        data.resize(texels * 2);
        for (usize t = 0; t < texels; ++t) {
            const u8* v = &g.values[t * 4];
            const u16 packed = u16((v[2] & 0xF) << 12 | (v[1] & 0xF) << 8 | (v[0] & 0xF) << 4 | (v[3] & 0xF));
            std::memcpy(&data[t * 2], &packed, 2);
        }
    } else {
        data.resize(texels * 4);
        const bool expand = set.latentFormat == assets::NtcLatentFormat::Q4;
        for (usize i = 0; i < texels * 4; ++i) data[i] = expand ? u8(g.values[i] * 17u) : g.values[i];
    }
    const rhi::TextureHandle t = device.createTexture(d);
    if (async) device.uploadTextureAsync(t, data, {0, ~0u, 0, ~0u, rhi::Access::SampledGraphics});
    else device.uploadTexture(t, data);
    if (bytes) *bytes = data.size();
    return t;
}

} // namespace ox::render::neural::detail
