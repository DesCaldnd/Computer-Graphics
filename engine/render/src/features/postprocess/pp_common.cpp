#include "pp_common.hpp"

namespace ox::render::pp {

rhi::TextureDesc texDesc(VkFormat format, u32 width, u32 height, const char* name, u32 mips) {
    rhi::TextureDesc d;
    d.format = format;
    d.width = std::max(width, 1u);
    d.height = std::max(height, 1u);
    d.mipLevels = mips;
    d.usage = rhi::TextureUsage::None; // derived by the graph
    d.name = name;
    return d;
}

void CasPass::init(rhi::Device& device) {
    if (!m_pipeline) m_pipeline = createComputePipeline(device, "pp.cas", "render/postprocess/sharpen_cas.comp");
}

void CasPass::shutdown(rhi::Device& device) {
    if (m_pipeline) device.destroy(m_pipeline);
    m_pipeline = {};
}

rhi::RGTexture CasPass::add(FeatureContext& ctx, rhi::RGTexture src, Extent2D size, f32 sharpness, const char* name) {
    const rhi::RGTexture dst = ctx.graph().createTexture(texDesc(formats::kSceneColor, size.width, size.height, name));
    const rhi::PipelineHandle pipe = m_pipeline;
    ctx.graph()
        .addPass(name, rhi::PassType::Compute)
        .read(src, rhi::Access::SampledCompute)
        .overwrite(dst, rhi::Access::StorageWriteCompute)
        .execute([=](rhi::PassContext& p) {
            struct {
                u32 src, dst;
                f32 sharpness;
                u32 pad;
                glm::vec2 size;
            } pc{p.sampledIndex(src), p.storageIndex(dst), sharpness, 0, {f32(size.width), f32(size.height)}};
            p.cmd.bindPipeline(pipe);
            p.cmd.pushConstants(pc);
            p.cmd.dispatchThreads(size.width, size.height);
        });
    return dst;
}

} // namespace ox::render::pp
