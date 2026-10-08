// "WorldGeometry" (AfterDepth: terrain + vegetation depth/normals/velocity merged into the core G-buffer, HiZ rebuilt;
// AfterOpaque: forward shading before the sky) and "WorldShadows" (Shadows: terrain + tree casters added to the sun
// cascades, ShadowMask rebuilt so the core opaque pass receives them).
#include "world_geometry.hpp"

#if OX_RENDER_HAS_WORLD

#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>

namespace ox::render::worldfx {

WorldGeometryShared::~WorldGeometryShared() = default;

// GPU objects are created on first use (a snapshot with terrain / vegetation), so renderers without a world pay
// nothing: no pipelines compiled, no built-in vegetation assets in the resource cache.
bool WorldGeometryShared::initialize(rhi::Device& dev, Renderer& renderer) {
    ++users;
    if (initialized) return true;
    m_device = &dev;
    m_renderer = &renderer;
    terrain = std::make_unique<TerrainSystem>();
    vegetation = std::make_unique<VegetationSystem>();
    initialized = true;
    return true;
}

void WorldGeometryShared::shutdown(rhi::Device& dev) {
    if (users > 0 && --users > 0) return;
    if (!initialized) return;
    if (terrainReady) terrain->shutdown(dev);
    if (vegetationReady) vegetation->shutdown(dev);
    if (hiz) dev.destroy(hiz);
    if (shadowMask) dev.destroy(shadowMask);
    hiz = shadowMask = {};
    terrain.reset();
    vegetation.reset();
    terrainReady = vegetationReady = false;
    initialized = false;
}

void WorldGeometryShared::update(FeatureContext& ctx, const WorldCVars& cv) {
    const u64 frame = ctx.stats().frame;
    if (updatedFrame == frame) return;
    updatedFrame = frame;
    const WorldSnapshot* world = ctx.snapshot().findExtension<WorldSnapshot>();
    rhi::Device& dev = ctx.device();
    if (world && !world->terrains.empty() && !terrainReady) terrainReady = terrain->initialize(dev);
    if (world && !world->vegetation.empty() && !vegetationReady) vegetationReady = vegetation->initialize(dev, *m_renderer);
    if ((terrainReady || vegetationReady) && !hiz) {
        hiz = createComputePipeline(dev, "world.hiz", "render/passes/hiz.comp");
        shadowMask = createFullscreenPipeline(dev, "world.shadowMask", "render/passes/shadow_mask.frag", {formats::kShadowMask});
    }
    if (terrainReady) terrain->update(ctx, cv.terrain ? world : nullptr, cv);
    if (vegetationReady) vegetation->update(ctx, cv.foliage ? world : nullptr, cv);
}

namespace {

struct WorldViewState final : IFeatureViewState {
    std::shared_ptr<TerrainSystem::ViewData> terrain;
    std::shared_ptr<VegetationSystem::ViewData> vegetation;
    u64 frame = ~0ull;
};

// Shared between the two features of a view: what was prepared at AfterDepth is drawn again at AfterOpaque.
struct ViewKey {
    std::shared_ptr<TerrainSystem::ViewData> terrain;
    std::shared_ptr<VegetationSystem::ViewData> vegetation;
};

class WorldGeometryFeature final : public IRenderFeature {
public:
    explicit WorldGeometryFeature(std::shared_ptr<WorldGeometryShared> s) : m_shared(std::move(s)) {}
    std::string_view name() const override { return "WorldGeometry"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::AfterDepth, InjectionPoint::AfterOpaque); }
    // First at AfterDepth (SSAO / SSR / decals must see the terrain), before the sky (-1000) at AfterOpaque.
    i32 order() const override { return -10000; }
    std::vector<std::string_view> provides() const override { return {res::kDepth, res::kNormals, res::kVelocity, res::kHiZ}; }
    std::vector<std::string> cvarNames() const override { return worldGeometryCVarNames(); }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override {
        const WorldCVars cv = WorldCVars::read();
        return cv.terrain || cv.foliage;
    }
    bool initialize(FeatureInitContext& ctx) override { return m_shared->initialize(ctx.device, ctx.renderer); }
    void shutdown(rhi::Device& dev) override { m_shared->shutdown(dev); }

    void setup(FeatureContext& ctx) override {
        if (ctx.point() == InjectionPoint::AfterDepth) setupPrepass(ctx);
        else setupForward(ctx);
    }

private:
    void setupPrepass(FeatureContext& ctx) {
        OX_PROFILE_ZONE_N("WorldGeometry.prepass");
        WorldViewState& vs = ctx.viewState<WorldViewState>();
        vs.terrain.reset();
        vs.vegetation.reset();
        const WorldCVars cv = WorldCVars::read();
        m_shared->update(ctx, cv);
        if (!ctx.snapshot().findExtension<WorldSnapshot>()) return;
        if (m_shared->vegetationReady) m_shared->vegetation->declareBakes(ctx);
        const RenderView& view = ctx.view();
        const glm::vec3 cam = view.camera().position();
        const glm::mat4 vp = view.unjitteredViewProj();
        if (cv.terrain && m_shared->terrainReady) vs.terrain = m_shared->terrain->prepare(ctx, cam, vp, false, cv);
        if (cv.foliage && m_shared->vegetationReady) {
            vs.vegetation = m_shared->vegetation->prepare(ctx, cam, std::span<const glm::mat4>(&vp, 1), false, cv);
        }
        vs.frame = ctx.stats().frame;
        if (!vs.terrain && !vs.vegetation) return;

        FrameResources& R = ctx.resources();
        const rhi::RGTexture depth = R.texture(res::kDepth), normals = R.texture(res::kNormals);
        const rhi::RGTexture velocity = R.texture(res::kVelocity), entity = R.texture(res::kEntityId);
        WorldPassContext wc;
        wc.view = ctx.viewAddress();
        wc.scene = ctx.sceneAddress();
        wc.entityIds = entity.valid();
        wc.sceneIndexBuffer = ctx.scene().indexBuffer();
        auto terrain = vs.terrain;
        auto veg = vs.vegetation;
        WorldGeometryShared* shared = m_shared.get();
        FrameState* fs = &frameState(ctx);
        rhi::PassBuilder pb = ctx.graph().addPass("World.Prepass");
        pb.color(normals, VK_ATTACHMENT_LOAD_OP_LOAD).color(velocity, VK_ATTACHMENT_LOAD_OP_LOAD);
        if (entity.valid()) pb.color(entity, VK_ATTACHMENT_LOAD_OP_LOAD);
        pb.depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD);
        if (veg) shared->vegetation->declareReads(pb, *veg);
        pb.execute([shared, terrain, veg, wc, fs, this](rhi::PassContext& p) {
            FeatureContext fc(*fs, this, InjectionPoint::AfterDepth);
            if (terrain) shared->terrain->draw(p.cmd, *terrain, WorldPass::Prepass, wc, fc);
            if (veg) shared->vegetation->draw(p, *veg, 0, WorldPass::Prepass, wc, fc);
        });

        // Rebuild HiZ with the world geometry (the core pyramid was built from the mesh-only depth; it is culled when
        // nothing reads it).
        if (R.texture(res::kHiZ).valid()) {
            const Extent2D re = ctx.renderExtent();
            const u32 mips = rhi::fullMipCount(re.width, re.height);
            rhi::TextureDesc hd;
            hd.format = formats::kHiZ;
            hd.width = re.width;
            hd.height = re.height;
            hd.mipLevels = mips;
            hd.usage = rhi::TextureUsage::None;
            hd.name = "HiZ";
            const rhi::RGTexture hiz = ctx.graph().createTexture(hd);
            const rhi::PipelineHandle pipe = shared->hiz;
            ctx.graph()
                .addPass("World.HiZ", rhi::PassType::Compute)
                .read(depth, rhi::Access::SampledCompute)
                .overwrite(hiz, rhi::Access::StorageWriteCompute)
                .execute([pipe, depth, hiz, re, mips](rhi::PassContext& p) {
                    struct {
                        u32 src, dst;
                        u32 srcSize[2], dstSize[2];
                        u32 fromDepth;
                    } pc{};
                    p.cmd.bindPipeline(pipe);
                    u32 w = re.width, h = re.height;
                    for (u32 mip = 0; mip < mips; ++mip) {
                        const u32 dw = mip == 0 ? w : std::max(w >> 1, 1u), dh = mip == 0 ? h : std::max(h >> 1, 1u);
                        pc.src = mip == 0 ? p.sampledIndex(depth) : p.storageIndex(hiz, mip - 1);
                        pc.dst = p.storageIndex(hiz, mip);
                        pc.srcSize[0] = w, pc.srcSize[1] = h, pc.dstSize[0] = dw, pc.dstSize[1] = dh;
                        pc.fromDepth = mip == 0 ? 1u : 0u;
                        p.cmd.pushConstants(pc);
                        p.cmd.dispatch((dw + 7) / 8, (dh + 7) / 8);
                        p.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
                        w = dw, h = dh;
                    }
                });
            R.setTexture(res::kHiZ, hiz);
        }
    }

    void setupForward(FeatureContext& ctx) {
        WorldViewState& vs = ctx.viewState<WorldViewState>();
        if (vs.frame != ctx.stats().frame || (!vs.terrain && !vs.vegetation)) return;
        FrameResources& R = ctx.resources();
        const rhi::RGTexture hdr = R.texture(res::kSceneColorHDR), depth = R.texture(res::kDepth);
        rhi::PassBuilder pb = ctx.graph().addPass("World.Forward");
        pb.color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD).depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, true);
        if (rhi::RGBuffer clusters = R.buffer(res::kLightClusters); clusters.valid()) pb.read(clusters, rhi::Access::StorageReadGraphics);
        const std::string_view inputNames[4] = {res::kShadowMask, res::kAO, res::kReflectionsSpecular, res::kIndirectDiffuse};
        std::array<rhi::RGTexture, 4> inputs{};
        for (u32 i = 0; i < 4; ++i) {
            inputs[i] = R.texture(inputNames[i]);
            if (inputs[i].valid()) pb.read(inputs[i], rhi::Access::SampledFragment);
        }
        for (std::string_view n : {res::kShadowCascades, res::kShadowAtlas, res::kPointShadows}) {
            if (rhi::RGTexture t = R.texture(n); t.valid()) pb.read(t, rhi::Access::SampledFragment);
        }
        auto terrain = vs.terrain;
        auto veg = vs.vegetation;
        if (veg) m_shared->vegetation->declareReads(pb, *veg);
        WorldPassContext wc;
        wc.view = ctx.viewAddress();
        wc.scene = ctx.sceneAddress();
        wc.sceneIndexBuffer = ctx.scene().indexBuffer();
        WorldGeometryShared* shared = m_shared.get();
        FrameState* fs = &frameState(ctx);
        pb.execute([shared, terrain, veg, wc, inputs, fs, this](rhi::PassContext& p) mutable {
            for (u32 i = 0; i < 4; ++i) wc.inputs[i] = inputs[i].valid() ? p.sampledIndex(inputs[i]) : kInvalidIndex;
            FeatureContext fc(*fs, this, InjectionPoint::AfterOpaque);
            if (terrain) shared->terrain->draw(p.cmd, *terrain, WorldPass::Forward, wc, fc);
            if (veg) shared->vegetation->draw(p, *veg, 0, WorldPass::Forward, wc, fc);
        });
    }

    std::shared_ptr<WorldGeometryShared> m_shared;
};

class WorldShadowsFeature final : public IRenderFeature {
public:
    explicit WorldShadowsFeature(std::shared_ptr<WorldGeometryShared> s) : m_shared(std::move(s)) {}
    std::string_view name() const override { return "WorldShadows"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Shadows); }
    i32 order() const override { return 100; } // after ShadowsRaster (cascades + matrices exist)
    std::vector<std::string_view> provides() const override { return {res::kShadowMask, res::kShadowCascades}; }
    std::vector<std::string> cvarNames() const override { return {"r.Terrain.Shadows", "r.Foliage.Shadows"}; }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps&) const override {
        const WorldCVars cv = WorldCVars::read();
        return s.shadows && ((cv.terrain && cv.terrainShadows) || (cv.foliage && cv.foliageShadows));
    }
    bool initialize(FeatureInitContext& ctx) override { return m_shared->initialize(ctx.device, ctx.renderer); }
    void shutdown(rhi::Device& dev) override { m_shared->shutdown(dev); }

    void setup(FeatureContext& ctx) override {
        OX_PROFILE_ZONE_N("WorldShadows");
        if (!ctx.snapshot().findExtension<WorldSnapshot>()) return;
        FrameResources& R = ctx.resources();
        GpuViewConstants& c = ctx.viewConstants();
        const rhi::RGTexture cascades = R.texture(res::kShadowCascades);
        if (!cascades.valid() || c.cascadeCount == 0 || c.sunLight < 0) return;
        const WorldCVars cv = WorldCVars::read();
        m_shared->update(ctx, cv);
        const glm::vec3 cam = ctx.view().camera().position();
        const u32 count = std::min(c.cascadeCount, kMaxCascades);
        std::vector<std::shared_ptr<TerrainSystem::ViewData>> terrain(count);
        bool any = false;
        if (cv.terrain && cv.terrainShadows && m_shared->terrainReady) {
            for (u32 i = 0; i < count; ++i) {
                terrain[i] = m_shared->terrain->prepare(ctx, cam, c.cascadeViewProj[i], true, cv);
                any |= terrain[i] != nullptr;
            }
        }
        std::shared_ptr<VegetationSystem::ViewData> veg;
        if (cv.foliage && cv.foliageShadows && m_shared->vegetationReady) {
            veg = m_shared->vegetation->prepare(ctx, cam, std::span<const glm::mat4>(c.cascadeViewProj, count), true, cv);
            any |= veg != nullptr;
        }
        if (!any) return;
        std::vector<VkDeviceAddress> matrices(count);
        for (u32 i = 0; i < count; ++i) matrices[i] = ctx.upload(std::span<const glm::mat4>(&c.cascadeViewProj[i], 1));
        const FrameState& fsc = frameState(ctx);
        const glm::vec3 sunDir = fsc.lights[usize(c.sunLight)].direction;
        WorldPassContext wc;
        wc.view = ctx.viewAddress();
        wc.scene = ctx.sceneAddress();
        wc.sceneIndexBuffer = ctx.scene().indexBuffer();
        wc.lightDirOct = packOctUnorm(sunDir);
        WorldGeometryShared* shared = m_shared.get();
        FrameState* fs = &frameState(ctx);
        rhi::PassBuilder pb = ctx.graph().addPass("World.ShadowCascades");
        pb.write(cascades, rhi::Access::DepthStencilWrite);
        if (veg) shared->vegetation->declareReads(pb, *veg);
        pb.execute([shared, terrain, veg, wc, matrices, cascades, count, fs, this](rhi::PassContext& p) mutable {
            FeatureContext fc(*fs, this, InjectionPoint::Shadows);
            const rhi::TextureHandle tex = p.texture(cascades);
            for (u32 i = 0; i < count; ++i) {
                rhi::RenderingDesc rd;
                rd.depth = rhi::DepthAttachment{tex, 0, i, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
                p.cmd.beginRendering(rd);
                p.cmd.setDepthBias(0.0f, 0.0f, -2.0f);
                wc.matrix = matrices[i];
                wc.cascade = i;
                if (terrain[i]) shared->terrain->draw(p.cmd, *terrain[i], WorldPass::Shadow, wc, fc);
                if (veg) shared->vegetation->draw(p, *veg, i, WorldPass::Shadow, wc, fc);
                p.cmd.endRendering();
            }
        });

        // Re-derive the screen-space sun visibility with the world casters (the core mask is then unused and culled).
        const rhi::RGTexture depth = R.texture(res::kDepth), normals = R.texture(res::kNormals);
        rhi::TextureDesc md;
        md.format = formats::kShadowMask;
        md.width = ctx.renderExtent().width;
        md.height = ctx.renderExtent().height;
        md.usage = rhi::TextureUsage::None;
        md.name = "ShadowMask";
        const rhi::RGTexture mask = ctx.graph().createTexture(md);
        const rhi::PipelineHandle maskPipe = shared->shadowMask;
        const VkDeviceAddress viewAddr = ctx.viewAddress(), sceneAddr = ctx.sceneAddress();
        ctx.graph()
            .addPass("World.ShadowMask")
            .read(depth, rhi::Access::SampledFragment)
            .read(normals, rhi::Access::SampledFragment)
            .read(cascades, rhi::Access::SampledFragment)
            .color(mask, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
            .execute([maskPipe, depth, normals, viewAddr, sceneAddr](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 depth, normals;
                } pc{viewAddr, sceneAddr, p.sampledIndex(depth), p.sampledIndex(normals)};
                drawFullscreen(p.cmd, maskPipe, &pc, sizeof(pc));
            });
        R.setTexture(res::kShadowMask, mask);
    }

private:
    std::shared_ptr<WorldGeometryShared> m_shared;
};

} // namespace

std::unique_ptr<IRenderFeature> makeWorldGeometryFeature(std::shared_ptr<WorldGeometryShared> shared) {
    return std::make_unique<WorldGeometryFeature>(std::move(shared));
}
std::unique_ptr<IRenderFeature> makeWorldShadowsFeature(std::shared_ptr<WorldGeometryShared> shared) {
    return std::make_unique<WorldShadowsFeature>(std::move(shared));
}

} // namespace ox::render::worldfx

#endif // OX_RENDER_HAS_WORLD
