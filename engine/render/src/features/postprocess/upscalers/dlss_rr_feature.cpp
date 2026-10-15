// NVIDIA DLSS Ray Reconstruction ("DLSS-D", NVSDK_NGX_Feature_RayReconstruction) as the upscaler of ray traced
// frames. Enabled by r.Denoiser = DLSSRR | Auto when ray tracing is active (path tracer off) and NGX reports the
// feature (nvngx_dlssd runtime, RTX GPU, recent driver). It takes the "Upscaler" slot with a higher priority than DLSS
// super resolution / TAAU / FSR 1 and, while it prepares a view, marks the view so that the ray traced effects skip
// their SVGF denoisers (rayReconstructionRunsThisFrame): the noisy lit frame goes to NGX together with
//   - hardware depth (reversed-Z), motion vectors (same conventions as DLSS SR), exposure + pre-exposure,
//   - Normals (world normal, roughness packed in w: NVSDK_NGX_DLSS_Roughness_Mode_Packed),
//   - diffuse / specular albedo and the reflection hit distance from the ray tracing area (RR.* resources; white /
//     black defaults if they are missing),
//   - the camera matrices (world → view, unjittered view → clip).
// RR denoises and upscales (or anti-aliases at 100 %, DLAA) in one network; the output is at output resolution like
// DLSS SR. Render resolution: r.Upscaler.Quality via the DLSSD optimal settings, DLAA when r.Upscaler is Off.
//
// Lifetime: one NGX feature per view, recreated on size / quality / flag changes (after waitIdle), released with the
// view, at shutdown, and by the companion feature "DLSSRRRelease" as soon as Ray Reconstruction is switched off, so
// toggling r.Denoiser at run time gives the NGX memory back.
#include "../pp_common.hpp"
#include "dlss_backend.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/render/render_settings.hpp>
#include <oxwald/render/render_view.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <tuple>

namespace ox::render::pp {

namespace {

class DlssRrFeature;

struct RrViewState final : IFeatureViewState {
    dlss::ViewFeature feature;
    DlssRrFeature* owner = nullptr;
    const RenderView* view = nullptr;
    void release(rhi::Device& device) override;
};

class DlssRrFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "DLSSRR"; }
    InjectionMask injectionPoints() const override {
        return maskOf(InjectionPoint::BeforePostProcess, InjectionPoint::Upscale);
    }
    std::string_view exclusiveGroup() const override { return "Upscaler"; }
    i32 priority() const override { return 400; } // above DLSS (300), TAAU (200), FSR 1 (100)
    std::vector<std::string_view> provides() const override { return {res::kSceneColorHDR}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.Denoiser", "r.Upscaler", "r.Upscaler.Quality"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return rayReconstructionSelected(s, caps);
    }

    bool initialize(FeatureInitContext& ctx) override {
        const dlss::Status st = dlss::probe(ctx.device);
        if (!st.rrAvailable) {
            OX_LOG_WARN("render", "DLSS Ray Reconstruction unavailable, the ray traced effects keep SVGF: {}", st.rrReason);
            return false;
        }
        m_device = &ctx.device;
        m_fallback = createComputePipeline(ctx.device, "pp.dlssrr.fallback", "render/postprocess/fsr1_easu.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        d.waitIdle();
        for (RrViewState* s : m_states) releaseState(*s);
        m_states.clear();
        if (m_fallback) d.destroy(m_fallback);
        m_fallback = {};
        m_optimal.clear();
        m_device = nullptr; // NGX itself stays up until the device is destroyed (dlss::probe)
    }

    void forget(RrViewState* s) { std::erase(m_states, s); }
    [[nodiscard]] bool hasLiveFeatures() const { return m_live.load() > 0; }
    // Ray Reconstruction is off for `view`: give its NGX feature back (DLSSRRRelease).
    void releaseView(rhi::Device& device, const RenderView& view) {
        bool waited = false;
        for (RrViewState* s : m_states) {
            if (s->view != &view || !s->feature.handle) continue;
            if (!waited) device.waitIdle(); // earlier frames may still evaluate it
            waited = true;
            releaseState(*s);
        }
        forgetRayReconstruction(view);
    }

    void prepareView(ViewSetup& setup) override {
        const UpscalerQuality q = rayReconstructionQuality(setup.settings);
        f32 scale = upscalerRenderScale(q);
        if (m_device && q != UpscalerQuality::Native && setup.outputExtent.width > 0) {
            const auto key = std::make_tuple(setup.outputExtent.width, setup.outputExtent.height, i32(q));
            auto it = m_optimal.find(key);
            if (it == m_optimal.end()) {
                it = m_optimal.emplace(key, dlss::rrOptimalSettings(*m_device, setup.outputExtent.width,
                                                                    setup.outputExtent.height, q)).first;
            }
            if (it->second.valid) scale = f32(it->second.renderWidth) / f32(setup.outputExtent.width);
        }
        setup.screenPercentage = scale * 100.0f;
        setup.jitterPhases = upscalerJitterPhases(scale);
        setup.mipBias += upscalerMipBias(UpscalerType::DLSS, scale);
        // From here on the ray traced effects of this view skip SVGF for this frame.
        markRayReconstruction(setup.view);
    }

    void setup(FeatureContext& ctx) override {
        if (ctx.point() == InjectionPoint::Upscale) return; // already at output resolution
        FrameResources& R = ctx.resources();
        const rhi::RGTexture color = R.texture(res::kSceneColorHDR);
        const rhi::RGTexture depth = R.texture(res::kDepth);
        const rhi::RGTexture velocity = R.texture(res::kVelocity);
        const rhi::RGTexture normals = R.texture(res::kNormals);
        if (!color.valid() || !depth.valid() || !velocity.valid() || !normals.valid()) return;
        const rhi::RGTexture exposure = R.texture(kExposure);
        const rhi::RGTexture diffuse = R.texture(res::kRrDiffuseAlbedo);
        const rhi::RGTexture specular = R.texture(res::kRrSpecularAlbedo);
        const rhi::RGTexture hitDistance = R.texture(res::kRrSpecularHitDistance);
        RrViewState& vs = ctx.viewState<RrViewState>();
        if (!vs.owner) {
            vs.owner = this;
            vs.view = &ctx.view();
            m_states.push_back(&vs);
        }
        const Extent2D in = ctx.renderExtent(), out = ctx.outputExtent();
        const rhi::RGTexture result =
            ctx.graph().createTexture(texDesc(formats::kSceneColor, out.width, out.height, "DLSSRR.Output"));
        dlss::RrEvaluateParams ep;
        ep.renderWidth = in.width;
        ep.renderHeight = in.height;
        ep.outputWidth = out.width;
        ep.outputHeight = out.height;
        ep.jitterPixels = ctx.view().jitterPixels();
        ep.worldToView = ctx.view().viewMatrix();
        ep.viewToClip = ctx.view().unjitteredProj();
        ep.preExposure = ctx.viewConstants().preExposure;
        ep.frameTimeMs = ctx.snapshot().deltaTime > 0.0f ? ctx.snapshot().deltaTime * 1000.0f : 16.6f;
        ep.reset = ctx.view().cameraCut();
        ep.quality = rayReconstructionQuality(ctx.settings());
        // Missing guides (ray tracing scene not ready this frame): neutral albedo, no hit distance.
        const rhi::TextureHandle white = ctx.defaults().white, black = ctx.defaults().black;
        dlss::ViewFeature* feature = &vs.feature;
        std::atomic<u32>* live = &m_live;
        rhi::Device* device = &ctx.device();
        const rhi::PipelineHandle fallback = m_fallback;
        rhi::PassBuilder pb = ctx.graph().addPass("DLSSRR", rhi::PassType::Compute);
        pb.read(color, rhi::Access::SampledCompute)
            .read(depth, rhi::Access::SampledCompute)
            .read(velocity, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .overwrite(result, rhi::Access::StorageWriteCompute);
        for (const rhi::RGTexture t : {exposure, diffuse, specular, hitDistance}) {
            if (t.valid()) pb.read(t, rhi::Access::SampledCompute);
        }
        pb.execute([=](rhi::PassContext& p) {
            dlss::RrEvaluateParams e = ep;
            e.color = p.texture(color);
            e.depth = p.texture(depth);
            e.motion = p.texture(velocity);
            e.normals = p.texture(normals);
            e.diffuseAlbedo = diffuse.valid() ? p.texture(diffuse) : white;
            e.specularAlbedo = specular.valid() ? p.texture(specular) : black;
            e.specularHitDistance = hitDistance.valid() ? p.texture(hitDistance) : rhi::TextureHandle{};
            e.exposure = exposure.valid() ? p.texture(exposure) : rhi::TextureHandle{};
            e.output = p.texture(result);
            const bool hadFeature = feature->handle != nullptr;
            const bool ok = dlss::evaluateRR(*device, p.cmd, *feature, e);
            if (!hadFeature && feature->handle) live->fetch_add(1);
            if (hadFeature && !feature->handle) live->fetch_sub(1); // recreation failed
            if (ok) return;
            // NGX failed this frame: spatial upscale of the (noisy) frame so it is still complete.
            struct {
                u32 src, dst, linearOut, pad;
                glm::vec4 inSize, outSize;
            } pc{p.sampledIndex(color), p.storageIndex(result), 1u, 0,
                 {f32(in.width), f32(in.height), 1.0f / f32(in.width), 1.0f / f32(in.height)},
                 {f32(out.width), f32(out.height), 1.0f / f32(out.width), 1.0f / f32(out.height)}};
            p.cmd.bindPipeline(fallback);
            p.cmd.pushConstants(pc);
            p.cmd.dispatchThreads(out.width, out.height);
        });
        R.setTexture(res::kSceneColorHDR, result);
    }

private:
    void releaseState(RrViewState& s) {
        if (s.feature.handle) m_live.fetch_sub(1);
        dlss::release(s.feature);
    }

    rhi::Device* m_device = nullptr;
    rhi::PipelineHandle m_fallback;
    std::vector<RrViewState*> m_states;
    std::atomic<u32> m_live{0}; // views holding an NGX feature
    std::map<std::tuple<u32, u32, i32>, dlss::OptimalSettings> m_optimal;

};

void RrViewState::release(rhi::Device& device) {
    if (!owner) return;
    device.waitIdle();
    if (feature.handle) owner->releaseView(device, *view);
    else forgetRayReconstruction(*view);
    owner->forget(this);
    owner = nullptr;
}

// Releases a view's Ray Reconstruction feature once RR is switched off (r.Denoiser, r.RayTracing, path tracer): the
// DLSSRR feature itself is no longer called then. Declares no passes.
class DlssRrReleaseFeature final : public IRenderFeature {
public:
    explicit DlssRrReleaseFeature(DlssRrFeature& rr) : m_rr(rr) {}
    std::string_view name() const override { return "DLSSRRRelease"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::BeforePostProcess); }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return m_rr.hasLiveFeatures() && !rayReconstructionSelected(s, caps);
    }
    void setup(FeatureContext& ctx) override { m_rr.releaseView(ctx.device(), ctx.view()); }

private:
    DlssRrFeature& m_rr;
};

} // namespace

std::unique_ptr<IRenderFeature> makeDlssRayReconstructionFeature() { return std::make_unique<DlssRrFeature>(); }

std::unique_ptr<IRenderFeature> makeDlssRayReconstructionReleaseFeature(IRenderFeature& rayReconstruction) {
    return std::make_unique<DlssRrReleaseFeature>(static_cast<DlssRrFeature&>(rayReconstruction));
}

} // namespace ox::render::pp
