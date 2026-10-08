// "Reflections" feature: reflection probes (capture → GGX prefilter → clustered lists → box-projected blending),
// planar reflections (mirrored oblique view), SSR (HiZClosest ray march, spatial resolve, temporal accumulation from
// the previous frame's colour pyramid) composited into ReflectionsSpecular.
#include "reflection_internal.hpp"

#include <oxwald/core/log.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

namespace ox::render::reflections {

namespace {

constexpr VkFormat kHdr = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr u32 kProbeEvictFrames = 300;

struct ProbeSlot {
    rhi::TextureHandle raw;         // capture cube (mip chain for filtered importance sampling)
    rhi::TextureHandle prefiltered; // GGX per mip
    u32 size = 0, rawMips = 0, mips = 0;
    bool captured = false;
    bool fromBaked = false;
    bool forceCapture = false;
    u64 paramsHash = 0;
    u64 lastSeen = 0;
    u32 nextFace = 0;
    Uuid uuid;
};

struct PlanarSlot {
    rhi::TextureHandle texture;
    Extent2D size;
};

struct ViewState final : IFeatureViewState {
    HistoryTexture color; // SSR source colour pyramid of this frame (current written at AfterOpaque)
    bool writeColor = false;
    u32 colorMips = 1;
    PlanarSlot planar[kMaxPlanarReflections];
    void release(rhi::Device& d) override {
        for (PlanarSlot& p : planar) {
            if (p.texture) d.destroy(p.texture);
            p = {};
        }
    }
};

u64 probeKey(const SnapshotReflectionProbe& p) {
    if (p.uuid.isValid()) return std::hash<Uuid>{}(p.uuid) & ~(1ull << 63);
    return (1ull << 63) | p.entityId;
}

u32 mipsForSize(u32 size) { return std::max(1u, u32(std::log2(f32(size))) - 1); } // smallest face 4×4

struct FrameProbe {
    GpuReflectionProbe gpu;
    rhi::TextureHandle cube;
    i32 priority = 0;
    f32 volume = 0.0f;
};

} // namespace

class ReflectionsFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return kReflectionsFeature; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting, InjectionPoint::AfterOpaque); }
    i32 order() const override { return 0; }
    std::string_view exclusiveGroup() const override { return "Reflections"; }
    std::vector<std::string_view> provides() const override {
        return {render::res::kReflectionsSpecular, res::kHiZClosest, res::kPlanarReflection, res::kSSR};
    }
    std::vector<std::string> cvarNames() const override { return cv::reflectionNames(); }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::Device& dev = ctx.device;
        m_capture.init(dev);
        m_downsample = createComputePipeline(dev, "render.reflections.cubeDownsample", "render/reflections/cube_downsample.comp");
        m_prefilter = createComputePipeline(dev, "render.reflections.prefilter", "render/ibl/prefilter.comp");
        m_probeCull = createComputePipeline(dev, "render.reflections.probeCull", "render/reflections/probe_cull.comp");
        m_composite = createComputePipeline(dev, "render.reflections.composite", "render/reflections/reflections_composite.comp");
        m_hiz = createComputePipeline(dev, "render.reflections.hizClosest", "render/reflections/hiz_closest.comp");
        m_ssrTrace = createComputePipeline(dev, "render.reflections.ssrTrace", "render/reflections/ssr_trace.comp");
        m_ssrResolve = createComputePipeline(dev, "render.reflections.ssrResolve", "render/reflections/ssr_resolve.comp");
        m_ssrTemporal = createComputePipeline(dev, "render.reflections.ssrTemporal", "render/reflections/ssr_temporal.comp");
        m_colorPyramid = createComputePipeline(dev, "render.reflections.colorPyramid", "render/reflections/color_pyramid.comp");
        m_device = &dev;
        return true;
    }

    void shutdown(rhi::Device& dev) override {
        m_capture.shutdown(dev);
        for (rhi::PipelineHandle p : {m_downsample, m_prefilter, m_probeCull, m_composite, m_hiz, m_ssrTrace, m_ssrResolve,
                                      m_ssrTemporal, m_colorPyramid}) {
            if (p) dev.destroy(p);
        }
        for (auto& [key, slot] : m_slots) destroySlot(dev, slot);
        m_slots.clear();
    }

    void setup(FeatureContext& ctx) override {
        if (ctx.point() == InjectionPoint::Lighting) setupLighting(ctx);
        else if (ctx.point() == InjectionPoint::AfterOpaque) setupAfterOpaque(ctx);
    }

    // --- bake API ---
    void requestBake() { m_bakeRequested = true; }
    // A request is consumed by the next frame that runs the feature.
    [[nodiscard]] bool bakeInProgress() const { return m_bakeRequested || m_pendingCaptures > 0; }
    void setBaked(const Uuid& id, BakedCubemap data) {
        m_baked[id] = std::move(data);
        for (auto& [key, slot] : m_slots) {
            if (slot.uuid == id) {
                slot.captured = false; // re-evaluated next frame: uploads the baked data
                slot.fromBaked = false;
            }
        }
    }
    std::vector<std::pair<Uuid, BakedCubemap>> readBaked(rhi::Device& dev) {
        std::vector<std::pair<Uuid, BakedCubemap>> out;
        dev.waitIdle();
        for (auto& [key, slot] : m_slots) {
            if (!slot.captured || !slot.uuid.isValid() || !slot.prefiltered) continue;
            BakedCubemap c;
            c.size = slot.size;
            c.mips = slot.mips;
            for (u32 m = 0; m < slot.mips; ++m) {
                for (u32 f = 0; f < 6; ++f) {
                    std::vector<u8> face = dev.readTexture(slot.prefiltered, m, f);
                    c.data.insert(c.data.end(), face.begin(), face.end());
                }
            }
            out.emplace_back(slot.uuid, std::move(c));
        }
        return out;
    }

private:
    void destroySlot(rhi::Device& dev, ProbeSlot& s) {
        if (s.raw) dev.destroy(s.raw);
        if (s.prefiltered) dev.destroy(s.prefiltered);
        s.raw = s.prefiltered = {};
    }

    void ensureSlotTextures(rhi::Device& dev, ProbeSlot& s, u32 size) {
        if (s.raw && s.size == size) return;
        destroySlot(dev, s);
        s.size = size;
        s.rawMips = rhi::fullMipCount(size, size);
        s.mips = mipsForSize(size);
        rhi::TextureDesc d;
        d.type = rhi::TextureType::Cube;
        d.arrayLayers = 6;
        d.format = kHdr;
        d.width = d.height = size;
        d.mipLevels = s.rawMips;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::ColorAttachment;
        d.name = "render.reflectionProbe.capture";
        s.raw = dev.createTexture(d);
        d.mipLevels = s.mips;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSrc |
                  rhi::TextureUsage::TransferDst;
        d.name = "render.reflectionProbe.prefiltered";
        s.prefiltered = dev.createTexture(d);
        s.captured = false;
        s.fromBaked = false;
    }

    // --- probes ---

    void updateProbes(FeatureContext& ctx, const ReflectionSnapshot* ext, ImportCache& imports) {
        rhi::Device& dev = ctx.device();
        const u64 frame = frameCounter(ctx) + 1; // 0 = never seen
        const bool bake = m_bakeRequested;
        m_bakeRequested = false;
        if (bake) {
            for (auto& [key, slot] : m_slots) slot.forceCapture = true;
        }
        u32 budget = u32(std::max(cv::probeCapturesPerFrame.get(), 1));
        u32 pending = 0;
        const u32 maxRes = std::bit_floor(u32(std::clamp(cv::probeResolution.get(), 16, 1024)));
        if (ext && cv::probes) {
            for (const SnapshotReflectionProbe& sp : ext->probes) {
                const u64 key = probeKey(sp);
                ProbeSlot& slot = m_slots[key];
                const bool first = slot.lastSeen == 0;
                const bool wasInactive = !first && slot.lastSeen + 1 < frame;
                slot.lastSeen = frame;
                slot.uuid = sp.uuid;
                if (bake && first) slot.forceCapture = true;
                const u32 size = std::clamp(std::bit_floor(std::max(sp.probe.resolution, 16u)), 16u, maxRes);
                ensureSlotTextures(dev, slot, size);
                u64 h = hashBytes(&sp.world, sizeof(sp.world));
                h = hashBytes(&sp.probe.captureOffset, sizeof(glm::vec3), h);
                h = hashBytes(&sp.probe.nearPlane, sizeof(f32), h);
                h = hashBytes(&sp.probe.farPlane, sizeof(f32), h);
                h = hashBytes(&size, sizeof(size), h);
                const bool paramsChanged = h != slot.paramsHash;

                const ReflectionProbeUpdate mode = sp.probe.update == ReflectionProbeUpdate::Realtime && !cv::probeRealtime
                                                       ? ReflectionProbeUpdate::OnEnable
                                                       : sp.probe.update;
                if (mode == ReflectionProbeUpdate::Baked && !slot.captured && !slot.forceCapture && sp.uuid.isValid()) {
                    auto it = m_baked.find(sp.uuid);
                    if (it != m_baked.end() && uploadBaked(dev, slot, it->second)) {
                        slot.paramsHash = h;
                        continue;
                    }
                }
                bool need = slot.forceCapture || !slot.captured || (paramsChanged && !slot.fromBaked);
                if (mode == ReflectionProbeUpdate::OnEnable && wasInactive) need = true;
                if (need) {
                    if (budget == 0) {
                        ++pending;
                        continue;
                    }
                    --budget;
                    captureProbe(ctx, slot, sp, {0, 1, 2, 3, 4, 5}, imports);
                    slot.captured = true;
                    slot.fromBaked = false;
                    slot.forceCapture = false;
                    slot.paramsHash = h;
                    slot.nextFace = 0;
                } else if (mode == ReflectionProbeUpdate::Realtime) {
                    const u32 n = u32(std::clamp(cv::probeRealtimeFaces.get(), 1, 6));
                    std::vector<u32> faces;
                    for (u32 i = 0; i < n; ++i) faces.push_back((slot.nextFace + i) % 6);
                    slot.nextFace = (slot.nextFace + n) % 6;
                    captureProbe(ctx, slot, sp, faces, imports);
                    slot.paramsHash = h;
                }
            }
        }
        m_pendingCaptures = pending;
        for (auto it = m_slots.begin(); it != m_slots.end();) {
            if (it->second.lastSeen + kProbeEvictFrames < frame) {
                destroySlot(dev, it->second);
                it = m_slots.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool uploadBaked(rhi::Device& dev, ProbeSlot& slot, const BakedCubemap& data) {
        if (!data.valid() || data.size != slot.size || data.mips != slot.mips) {
            OX_LOG_WARN("render", "baked reflection probe {} does not match the probe ({}^2 x {} mips vs {}^2 x {}): re-capturing",
                        slot.uuid.toString(), data.size, data.mips, slot.size, slot.mips);
            return false;
        }
        rhi::TextureUploadDesc u;
        u.finalAccess = rhi::Access::SampledCompute;
        dev.waitIdle(); // frames in flight may still sample the cube
        dev.uploadTexture(slot.prefiltered, data.data, u);
        slot.captured = true;
        slot.fromBaked = true;
        return true;
    }

    void captureProbe(FeatureContext& ctx, ProbeSlot& slot, const SnapshotReflectionProbe& sp, std::vector<u32> faces,
                      ImportCache& imports) {
        const rhi::RGTexture raw = imports.get(ctx, slot.raw);
        const rhi::RGTexture pref = imports.get(ctx, slot.prefiltered);
        const glm::vec3 pos = glm::vec3(sp.world * glm::vec4(sp.probe.captureOffset, 1.0f));
        const f32 nearPlane = std::max(sp.probe.nearPlane, 1e-3f);
        for (u32 face : faces) {
            glm::mat4 view, proj;
            cubeFaceMatrices(face, pos, nearPlane, view, proj);
            SceneCapture::Request rq;
            rq.name = "Reflections.ProbeCapture";
            rq.target = raw;
            rq.layer = face;
            rq.size = {slot.size, slot.size};
            rq.constants = SceneCapture::makeConstants(ctx.viewConstants(), view, proj, pos, rq.size, 1.0f);
            rq.filter.frustum = Frustum::fromViewProj(proj * view, true);
            rq.filter.sphere = Sphere{pos, std::max(sp.probe.farPlane, nearPlane * 2.0f)};
            m_capture.addPass(ctx, rq);
        }
        const u32 size = slot.size, rawMips = slot.rawMips, mips = slot.mips;
        const rhi::PipelineHandle down = m_downsample, prefilter = m_prefilter;
        ctx.graph()
            .addPass("Reflections.ProbeMips", rhi::PassType::Compute)
            .write(raw, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                p.cmd.bindPipeline(down);
                for (u32 m = 1; m < rawMips; ++m) {
                    const u32 s = std::max(size >> m, 1u);
                    struct {
                        u32 src, dst, dstSize;
                    } pc{p.storageIndex(raw, m - 1), p.storageIndex(raw, m), s};
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatch((s + 7) / 8, (s + 7) / 8, 6);
                    p.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
                }
            });
        ctx.graph()
            .addPass("Reflections.ProbePrefilter", rhi::PassType::Compute)
            .read(raw, rhi::Access::SampledCompute)
            .overwrite(pref, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                p.cmd.bindPipeline(prefilter);
                for (u32 mip = 0; mip < mips; ++mip) {
                    const u32 s = std::max(size >> mip, 1u);
                    struct {
                        u32 src, dst, size, srcSize;
                        f32 roughness;
                        u32 samples;
                        f32 srcMips;
                    } pc{p.sampledIndex(raw), p.storageIndex(pref, mip), s, size,
                         mips > 1 ? f32(mip) / f32(mips - 1) : 0.0f, mip == 0 ? 1u : 64u, f32(rawMips)};
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatch((s + 7) / 8, (s + 7) / 8, 6);
                }
            });
    }

    std::vector<FrameProbe> visibleProbes(FeatureContext& ctx, const ReflectionSnapshot& ext) {
        std::vector<FrameProbe> out;
        rhi::Device& dev = ctx.device();
        const Frustum frustum = ctx.view().frustum();
        for (const SnapshotReflectionProbe& sp : ext.probes) {
            auto it = m_slots.find(probeKey(sp));
            if (it == m_slots.end() || !it->second.captured) continue;
            const ProbeSlot& slot = it->second;
            const glm::mat4 rigid = removeScale(sp.world);
            const glm::vec3 extents = glm::max(sp.probe.extents, glm::vec3(0.01f));
            const f32 radius = glm::length(extents) + std::max(sp.probe.blendDistance, 0.0f);
            const glm::vec3 center(rigid[3]);
            if (!frustum.intersects(Sphere{center, radius})) continue;
            FrameProbe fp;
            fp.gpu.worldToLocal = glm::inverse(rigid);
            fp.gpu.extents = extents;
            fp.gpu.blendDistance = std::max(sp.probe.blendDistance, 1e-3f);
            fp.gpu.capturePosition = glm::vec3(sp.world * glm::vec4(sp.probe.captureOffset, 1.0f));
            fp.gpu.intensity = sp.probe.intensity;
            fp.gpu.boundingSphere = glm::vec4(center, radius);
            fp.gpu.cube = dev.sampledIndex(slot.prefiltered);
            fp.gpu.mips = slot.mips;
            fp.gpu.flags = sp.probe.boxProjection ? kProbeBoxProjection : 0u;
            fp.cube = slot.prefiltered;
            fp.priority = sp.probe.priority;
            fp.volume = extents.x * extents.y * extents.z;
            out.push_back(fp);
        }
        std::stable_sort(out.begin(), out.end(), [](const FrameProbe& a, const FrameProbe& b) {
            return a.priority != b.priority ? a.priority > b.priority : a.volume < b.volume;
        });
        if (out.size() > kMaxReflectionProbes) out.resize(kMaxReflectionProbes);
        return out;
    }

    // --- planar ---

    u32 setupPlanar(FeatureContext& ctx, const ReflectionSnapshot* ext, ViewState& vs, ImportCache& imports,
                    std::vector<rhi::RGTexture>& textures) {
        if (!ext || ext->planars.empty() || !cv::planar || cv::planarMax.get() <= 0) return 0;
        const RenderView& view = ctx.view();
        if (view.camera().projection == CameraParams::Projection::Orthographic) return 0;
        rhi::Device& dev = ctx.device();
        const glm::vec3 camPos = view.camera().position();
        const Frustum frustum = view.frustum();
        struct Candidate {
            const SnapshotPlanarReflector* sp;
            glm::vec4 plane;
            glm::vec3 axisX, axisZ, origin;
            f32 halfX, halfZ;
            f32 distance;
        };
        std::vector<Candidate> cands;
        for (const SnapshotPlanarReflector& sp : ext->planars) {
            const glm::vec3 up(sp.world[1]);
            if (glm::length(up) < 1e-6f) continue;
            Candidate c;
            c.sp = &sp;
            const glm::vec3 n = glm::normalize(up);
            c.origin = glm::vec3(sp.world[3]);
            c.plane = glm::vec4(n, -glm::dot(n, c.origin));
            const f32 camSide = glm::dot(n, camPos) + c.plane.w;
            if (camSide <= 1e-3f) continue; // viewer behind the mirror
            const glm::vec3 ax(sp.world[0]), az(sp.world[2]);
            c.axisX = glm::length(ax) > 1e-6f ? glm::normalize(ax) : glm::vec3(1, 0, 0);
            c.axisZ = glm::length(az) > 1e-6f ? glm::normalize(az) : glm::vec3(0, 0, 1);
            c.halfX = sp.reflector.size.x > 0.0f ? sp.reflector.size.x * glm::length(ax) : 0.0f;
            c.halfZ = sp.reflector.size.y > 0.0f ? sp.reflector.size.y * glm::length(az) : 0.0f;
            if (c.halfX > 0.0f && c.halfZ > 0.0f &&
                !frustum.intersects(Sphere{c.origin, std::sqrt(c.halfX * c.halfX + c.halfZ * c.halfZ)})) {
                continue;
            }
            c.distance = camSide;
            cands.push_back(c);
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
            return a.sp->reflector.priority != b.sp->reflector.priority ? a.sp->reflector.priority > b.sp->reflector.priority
                                                                        : a.distance < b.distance;
        });
        const u32 maxCount = std::min<u32>(u32(cv::planarMax.get()), kMaxPlanarReflections);
        GpuPlanarReflections gpu;
        const GpuViewConstants& mainC = ctx.viewConstants();
        const Extent2D re = ctx.renderExtent();
        u32 count = 0;
        for (usize ci = 0; ci < cands.size() && count < maxCount; ++ci) {
            const Candidate& c = cands[ci];
            const PlanarReflectorComponent& comp = c.sp->reflector;
            const f32 scale = std::clamp(cv::planarScale.get() * comp.resolutionScale, 0.05f, 2.0f);
            const Extent2D size{std::max(16u, u32(f32(re.width) * scale)), std::max(16u, u32(f32(re.height) * scale))};
            PlanarSlot& slot = vs.planar[count];
            if (!slot.texture || slot.size != size) {
                if (slot.texture) dev.destroy(slot.texture);
                rhi::TextureDesc d = textureDesc(kHdr, size, "render.planarReflection");
                d.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
                slot.texture = dev.createTexture(d);
                slot.size = size;
            }
            const glm::mat4 reflView = view.viewMatrix() * reflectionMatrix(c.plane);
            const glm::vec3 reflCam = glm::vec3(reflectionMatrix(c.plane) * glm::vec4(camPos, 1.0f));
            const glm::vec4 clipWorld(glm::vec3(c.plane), c.plane.w - comp.clipOffset);
            const glm::vec4 clipView = glm::transpose(glm::inverse(reflView)) * clipWorld;
            const f32 nearPlane = std::max(view.camera().nearPlane, 0.01f);
            const glm::mat4 finite = finiteReversedZ(view.unjitteredProj(), nearPlane, std::max(comp.maxDistance, nearPlane * 4.0f));
            const glm::mat4 proj = obliqueReversedZ(finite, clipView);

            const rhi::RGTexture tex = imports.get(ctx, slot.texture);
            SceneCapture::Request rq;
            rq.name = "Reflections.Planar";
            rq.target = tex;
            rq.size = size;
            rq.constants = SceneCapture::makeConstants(mainC, reflView, proj, reflCam, size, mainC.preExposure);
            rq.filter.frustum = Frustum::fromViewProj(proj * reflView, true);
            rq.sunDisk = true;
            if (c.halfX > 0.0f && c.halfZ > 0.0f) {
                // Only the reflector's screen footprint is ever sampled: scissor the render to it.
                glm::vec2 lo(1e9f), hi(-1e9f);
                bool behind = false;
                for (int k = 0; k < 4; ++k) {
                    const glm::vec3 corner = c.origin + c.axisX * (c.halfX * ((k & 1) ? 1.0f : -1.0f)) +
                                             c.axisZ * (c.halfZ * ((k & 2) ? 1.0f : -1.0f));
                    const glm::vec4 clip = view.unjitteredViewProj() * glm::vec4(corner, 1.0f);
                    if (clip.w <= 1e-4f) {
                        behind = true;
                        break;
                    }
                    const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
                    lo = glm::min(lo, uv);
                    hi = glm::max(hi, uv);
                }
                if (!behind) {
                    const f32 margin = 0.02f + comp.distortion;
                    lo = glm::clamp(lo - margin, 0.0f, 1.0f);
                    hi = glm::clamp(hi + margin, 0.0f, 1.0f);
                    if (hi.x <= lo.x || hi.y <= lo.y) continue; // off screen
                    const u32 x0 = u32(lo.x * f32(size.width)), y0 = u32(lo.y * f32(size.height));
                    const u32 x1 = std::min(size.width, u32(std::ceil(hi.x * f32(size.width))));
                    const u32 y1 = std::min(size.height, u32(std::ceil(hi.y * f32(size.height))));
                    rq.scissor = {x0, y0, std::max(x1 - x0, 1u), std::max(y1 - y0, 1u)};
                }
            }
            m_capture.addPass(ctx, rq);
            textures.push_back(tex);

            GpuPlanarReflection& g = gpu.reflectors[count];
            g.plane = c.plane;
            g.axisX = glm::vec4(c.axisX, c.halfX);
            g.axisZ = glm::vec4(c.axisZ, c.halfZ);
            g.origin = glm::vec4(c.origin, comp.intensity);
            g.params = glm::vec4(comp.distortion, comp.maxRoughness, scale, 0.0f);
            g.texture = dev.sampledIndex(slot.texture);
            ++count;
        }
        if (count == 0) return 0;
        gpu.count = count;
        ctx.viewConstants().planarReflections = ctx.upload(std::span<const GpuPlanarReflections>(&gpu, 1));
        ctx.resources().setTexture(res::kPlanarReflection, textures[0]);
        return count;
    }

    // --- SSR ---

    rhi::RGTexture setupSsr(FeatureContext& ctx, ViewState& vs) {
        if (!cv::ssr) return {};
        FrameResources& R = ctx.resources();
        rhi::RenderGraph& g = ctx.graph();
        const Extent2D re = ctx.renderExtent();
        const rhi::RGTexture depth = R.texture(render::res::kDepth), normals = R.texture(render::res::kNormals);
        const rhi::RGTexture velocity = R.texture(render::res::kVelocity);
        const VkDeviceAddress viewAddr = ctx.viewAddress(), sceneAddr = ctx.sceneAddress();

        // HiZClosest.
        const u32 hizMips = rhi::fullMipCount(re.width, re.height);
        const rhi::RGTexture hiz = g.createTexture(textureDesc(VK_FORMAT_R32_SFLOAT, re, "HiZClosest", hizMips));
        const rhi::PipelineHandle hizPipe = m_hiz;
        g.addPass("Reflections.HiZClosest", rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .overwrite(hiz, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u32 src, dst;
                    u32 srcSize[2], dstSize[2];
                    u32 fromDepth;
                } pc{};
                p.cmd.bindPipeline(hizPipe);
                u32 w = re.width, h = re.height;
                for (u32 mip = 0; mip < hizMips; ++mip) {
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
        R.setTexture(res::kHiZClosest, hiz);

        // Source colour of the previous frame.
        vs.colorMips = std::min(rhi::fullMipCount(re.width, re.height), 8u);
        rhi::TextureDesc cd = textureDesc(kHdr, re, "Reflections.SSRColor", vs.colorMips);
        cd.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
        vs.color = ctx.history("Reflections.SSRColor", cd);
        vs.writeColor = true;
        if (!vs.color.previousValid) return {};

        const u32 quality = u32(std::clamp(cv::ssrQuality.get(), 0, 3));
        const u32 ds = cv::ssrHalfRes ? 2u : 1u;
        const Extent2D te{(re.width + ds - 1) / ds, (re.height + ds - 1) / ds};
        const rhi::RGTexture hits = g.createTexture(textureDesc(kHdr, te, "Reflections.SSRHits"));
        const u32 maxSteps = u32(std::clamp(cv::ssrMaxSteps.get(), 4, 512));
        const f32 maxRough = std::clamp(cv::ssrMaxRoughness.get(), 0.0f, 1.0f);
        const f32 thickness = cv::ssrThickness.get();
        const rhi::PipelineHandle trace = m_ssrTrace, resolvePipe = m_ssrResolve, temporalPipe = m_ssrTemporal;
        g.addPass("Reflections.SSRTrace", rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .read(hiz, rhi::Access::SampledCompute)
            .overwrite(hits, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 depth, normals, hiz, hizMips, out, downscale, maxSteps;
                    f32 maxRoughness, thickness;
                    u32 w, h, pad;
                } pc{viewAddr, sceneAddr, p.sampledIndex(depth), p.sampledIndex(normals), p.sampledIndex(hiz), hizMips,
                     p.storageIndex(hits), ds, maxSteps, maxRough, thickness, te.width, te.height, 0};
                p.cmd.bindPipeline(trace);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(te.width, te.height);
            });

        const rhi::RGTexture resolved = g.createTexture(textureDesc(kHdr, re, "Reflections.SSRResolve"));
        const rhi::RGTexture prevColor = vs.color.previous;
        const u32 colorMips = vs.colorMips;
        const u32 samples = quality == 0 ? 1u : quality == 3 ? 8u : 4u;
        g.addPass("Reflections.SSRResolve", rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .read(velocity, rhi::Access::SampledCompute)
            .read(hits, rhi::Access::SampledCompute)
            .read(prevColor, rhi::Access::SampledCompute)
            .overwrite(resolved, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 depth, normals, velocity, hits, color, colorMips, out, downscale, samples;
                    f32 maxRoughness;
                    u32 w, h;
                } pc{viewAddr, sceneAddr, p.sampledIndex(depth), p.sampledIndex(normals), p.sampledIndex(velocity),
                     p.sampledIndex(hits), p.sampledIndex(prevColor), colorMips, p.storageIndex(resolved), ds, samples,
                     maxRough, te.width, te.height};
                p.cmd.bindPipeline(resolvePipe);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(re.width, re.height);
            });
        rhi::RGTexture result = resolved;
        if (cv::ssrTemporal) {
            rhi::TextureDesc hd = textureDesc(kHdr, re, "Reflections.SSRTemporal");
            hd.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
            const HistoryTexture hist = ctx.history("Reflections.SSRTemporal", hd);
            const f32 blend = quality == 0 ? 0.25f : quality == 1 ? 0.15f : quality == 2 ? 0.1f : 0.08f;
            const u32 valid = hist.previousValid ? 1u : 0u;
            g.addPass("Reflections.SSRTemporal", rhi::PassType::Compute)
                .read(resolved, rhi::Access::SampledCompute)
                .read(hist.previous, rhi::Access::SampledCompute)
                .read(velocity, rhi::Access::SampledCompute)
                .read(depth, rhi::Access::SampledCompute)
                .overwrite(hist.current, rhi::Access::StorageWriteCompute)
                .execute([=](rhi::PassContext& p) {
                    struct {
                        u64 view, scene;
                        u32 current, history, velocity, depth, out, valid;
                        f32 blend;
                    } pc{viewAddr, sceneAddr, p.sampledIndex(resolved), p.sampledIndex(hist.previous), p.sampledIndex(velocity),
                         p.sampledIndex(depth), p.storageIndex(hist.current), valid, blend};
                    p.cmd.bindPipeline(temporalPipe);
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatchThreads(re.width, re.height);
                });
            result = hist.current;
        }
        R.setTexture(res::kSSR, result);
        return result;
    }

    void setupAfterOpaque(FeatureContext& ctx) {
        ViewState& vs = ctx.viewState<ViewState>();
        if (!vs.writeColor) return;
        vs.writeColor = false;
        const rhi::RGTexture hdr = ctx.resources().texture(render::res::kSceneColorHDR);
        if (!hdr.valid()) return;
        const rhi::RGTexture dst = vs.color.current;
        const Extent2D re = ctx.renderExtent();
        const u32 mips = vs.colorMips;
        const VkDeviceAddress viewAddr = ctx.viewAddress(), sceneAddr = ctx.sceneAddress();
        const rhi::PipelineHandle pipe = m_colorPyramid;
        ctx.graph()
            .addPass("Reflections.SSRColor", rhi::PassType::Compute)
            .read(hdr, rhi::Access::SampledCompute)
            .overwrite(dst, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 src, dst;
                    u32 srcSize[2], dstSize[2];
                    u32 fromScene;
                } pc{};
                pc.view = viewAddr;
                pc.scene = sceneAddr;
                p.cmd.bindPipeline(pipe);
                u32 w = re.width, h = re.height;
                for (u32 mip = 0; mip < mips; ++mip) {
                    const u32 dw = mip == 0 ? w : std::max(w >> 1, 1u), dh = mip == 0 ? h : std::max(h >> 1, 1u);
                    pc.src = mip == 0 ? p.sampledIndex(hdr) : p.storageIndex(dst, mip - 1);
                    pc.dst = p.storageIndex(dst, mip);
                    pc.srcSize[0] = w, pc.srcSize[1] = h, pc.dstSize[0] = dw, pc.dstSize[1] = dh;
                    pc.fromScene = mip == 0 ? 1u : 0u;
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatch((dw + 7) / 8, (dh + 7) / 8);
                    p.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
                    w = dw, h = dh;
                }
            });
    }

    // --- frame ---

    void setupLighting(FeatureContext& ctx) {
        const ReflectionSnapshot* ext = ctx.snapshot().findExtension<ReflectionSnapshot>();
        ViewState& vs = ctx.viewState<ViewState>();
        vs.writeColor = false;
        ImportCache imports;
        if (m_lastFrame != frameCounter(ctx)) {
            m_lastFrame = frameCounter(ctx);
            updateProbes(ctx, ext, imports);
        }
        std::vector<FrameProbe> probes;
        if (ext && cv::probes) probes = visibleProbes(ctx, *ext);
        std::vector<rhi::RGTexture> planarTex;
        const u32 planarCount = setupPlanar(ctx, ext, vs, imports, planarTex);
        const rhi::RGTexture ssr = setupSsr(ctx, vs);
        if (probes.empty() && planarCount == 0 && !ssr.valid()) return;

        FrameResources& R = ctx.resources();
        rhi::RenderGraph& g = ctx.graph();
        const Extent2D re = ctx.renderExtent();
        const rhi::RGTexture depth = R.texture(render::res::kDepth), normals = R.texture(render::res::kNormals);
        const VkDeviceAddress viewAddr = ctx.viewAddress(), sceneAddr = ctx.sceneAddress();
        const GpuViewConstants& c = ctx.viewConstants();
        const u32 clusterCount = c.clusterGrid.x * c.clusterGrid.y * c.clusterGrid.z;

        VkDeviceAddress probeAddr = 0;
        rhi::RGBuffer lists;
        const u32 probeCount = u32(probes.size());
        if (probeCount > 0) {
            std::vector<GpuReflectionProbe> gpu;
            gpu.reserve(probes.size());
            for (const FrameProbe& fp : probes) gpu.push_back(fp.gpu);
            probeAddr = ctx.upload(std::span<const GpuReflectionProbe>(gpu));
            lists = g.createBuffer({u64(clusterCount) * (1 + kMaxReflectionProbesPerCluster) * 4, rhi::BufferUsage::Storage,
                                    rhi::MemoryUsage::GpuOnly, "Reflections.ProbeClusters"});
            const rhi::PipelineHandle cull = m_probeCull;
            g.addPass("Reflections.ProbeCull", rhi::PassType::Compute)
                .overwrite(lists, rhi::Access::StorageWriteCompute)
                .execute([=](rhi::PassContext& p) {
                    struct {
                        u64 view, scene, probes, lists;
                        u32 count, pad;
                    } pc{viewAddr, sceneAddr, probeAddr, p.address(lists), probeCount, 0};
                    p.cmd.bindPipeline(cull);
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatch((clusterCount + 63) / 64);
                });
        }

        const rhi::RGTexture out = g.createTexture(textureDesc(formats::kReflections, re, "ReflectionsSpecular"));
        rhi::PassBuilder pb = g.addPass("Reflections.Composite", rhi::PassType::Compute);
        pb.read(depth, rhi::Access::SampledCompute).read(normals, rhi::Access::SampledCompute);
        if (lists.valid()) pb.read(lists, rhi::Access::StorageReadCompute);
        std::vector<u64> declared;
        for (const FrameProbe& fp : probes) {
            if (std::find(declared.begin(), declared.end(), fp.cube.packed()) != declared.end()) continue;
            declared.push_back(fp.cube.packed());
            pb.read(imports.get(ctx, fp.cube), rhi::Access::SampledCompute);
        }
        for (rhi::RGTexture t : planarTex) pb.read(t, rhi::Access::SampledCompute);
        if (ssr.valid()) pb.read(ssr, rhi::Access::SampledCompute);
        pb.overwrite(out, rhi::Access::StorageWriteCompute);
        const u32 maxPerPixel = u32(std::clamp(cv::probeMaxPerPixel.get(), 1, 16));
        const u32 flags = planarCount > 0 ? 1u : 0u;
        const rhi::PipelineHandle composite = m_composite;
        pb.execute([=](rhi::PassContext& p) {
            struct {
                u64 view, scene;
                u32 depth, normals, out, ssr;
                u64 probes, lists;
                u32 probeCount, maxPerPixel, flags;
            } pc{viewAddr, sceneAddr, p.sampledIndex(depth), p.sampledIndex(normals), p.storageIndex(out),
                 ssr.valid() ? p.sampledIndex(ssr) : kInvalidIndex, probeAddr, lists.valid() ? p.address(lists) : 0,
                 probeCount, maxPerPixel, flags};
            p.cmd.bindPipeline(composite);
            p.cmd.pushConstants(pc);
            p.cmd.dispatchThreads(re.width, re.height);
        });
        R.setTexture(render::res::kReflectionsSpecular, out);
    }

    SceneCapture m_capture;
    rhi::PipelineHandle m_downsample, m_prefilter, m_probeCull, m_composite, m_hiz, m_ssrTrace, m_ssrResolve, m_ssrTemporal,
        m_colorPyramid;
    rhi::Device* m_device = nullptr;
    std::unordered_map<u64, ProbeSlot> m_slots;
    std::unordered_map<Uuid, BakedCubemap> m_baked;
    u64 m_lastFrame = ~0ull;
    bool m_bakeRequested = false;
    u32 m_pendingCaptures = 0;
};

std::unique_ptr<IRenderFeature> makeReflectionsFeature() { return std::make_unique<ReflectionsFeature>(); }

ReflectionsFeature* findReflections(Renderer& renderer) {
    return dynamic_cast<ReflectionsFeature*>(renderer.features().find(kReflectionsFeature));
}

// --- public bake API (probes) -------------------------------------------------------------------------------------

void probesRequestBake(Renderer& renderer) {
    if (ReflectionsFeature* f = findReflections(renderer)) f->requestBake();
}

bool probesBakeInProgress(Renderer& renderer) {
    ReflectionsFeature* f = findReflections(renderer);
    return f && f->bakeInProgress();
}

std::vector<std::pair<Uuid, BakedCubemap>> readBakedProbes(Renderer& renderer) {
    ReflectionsFeature* f = findReflections(renderer);
    return f ? f->readBaked(renderer.device()) : std::vector<std::pair<Uuid, BakedCubemap>>{};
}

void setBakedProbe(Renderer& renderer, const Uuid& probe, BakedCubemap data) {
    if (ReflectionsFeature* f = findReflections(renderer)) f->setBaked(probe, std::move(data));
}

} // namespace ox::render::reflections
