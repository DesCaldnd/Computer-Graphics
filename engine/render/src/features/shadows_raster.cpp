// Built-in feature "ShadowsRaster" (exclusive group "Shadows"): directional CSM, spot lights in an atlas sized by
// importance, point lights as 6 faces in a 2D array (layered rendering through gl_Layer when the device supports
// vertex-shader layer output, else one pass per face), cached local light shadows, screen-space ShadowMask.
#include "../renderer_impl.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/render/shadows.hpp>

#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace ox::render {

namespace {

struct ShadowPush {
    u64 view = 0;
    u64 scene = 0;
    u64 drawIds = 0;
    u64 matrices = 0;
    u32 layerBase = 0;
    u32 pad = 0;
};

struct CacheEntry {
    u32 resolution = 0;
    ShadowAtlasAllocator::Tile tile;
    u32 cubeSlot = ~0u;
    glm::vec3 position{0.0f}, direction{0.0f};
    f32 range = 0.0f, outer = 0.0f;
    u64 lastFrame = 0;
};

class ShadowsRasterFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "ShadowsRaster"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Shadows); }
    std::string_view exclusiveGroup() const override { return "Shadows"; }
    std::vector<std::string_view> provides() const override {
        return {res::kShadowMask, res::kShadowCascades, res::kShadowAtlas, res::kPointShadows};
    }
    std::vector<std::string> cvarNames() const override {
        return {"r.Shadows",           "r.Shadows.CSM.Resolution",    "r.Shadows.CSM.Cascades", "r.Shadows.CSM.Distance",
                "r.Shadows.CSM.Lambda", "r.Shadows.CSM.Blend",        "r.Shadows.AtlasSize",    "r.Shadows.PointResolution",
                "r.Shadows.MaxShadowedLights", "r.Shadows.MaxPointShadows", "r.Shadows.PCFTaps", "r.Shadows.PCSS",
                "r.Shadows.FilterRadius", "r.Shadows.SpotMinResolution", "r.Shadows.SpotMaxResolution", "r.Shadows.Caching"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps&) const override { return s.shadows; }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::Device& dev = ctx.device;
        m_layered = dev.caps().shaderOutputLayer;
        for (u32 alpha = 0; alpha < 2; ++alpha) {
            for (u32 kind = 0; kind < 3; ++kind) { // 0 cascade, 1 spot / point face, 2 layered point
                if (kind == 2 && !m_layered) continue;
                rhi::GraphicsPipelineDesc d;
                d.name = std::format("render.shadow.{}{}", kind == 0 ? "cascade" : kind == 1 ? "local" : "layered",
                                     alpha ? ".alpha" : "");
                std::vector<rhi::ShaderDefine> defs;
                if (kind == 2) defs.push_back({"OX_LAYERED"});
                d.vertex = rhi::ShaderStageDesc::file("render/passes/shadow.vert", defs);
                if (alpha) d.fragment = rhi::ShaderStageDesc::file("render/passes/shadow_alpha.frag");
                d.depthFormat = formats::kShadowDepth;
                d.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
                d.raster.cullMode = VK_CULL_MODE_NONE;
                d.raster.depthBias = true;
                d.raster.depthClamp = kind == 0 && dev.caps().depthClamp; // pancaking for off-screen casters
                m_pipes[kind][alpha] = dev.createGraphicsPipeline(d);
            }
        }
        m_mask = createFullscreenPipeline(dev, "render.shadowMask", "render/passes/shadow_mask.frag", {formats::kShadowMask});
        return true;
    }

    void shutdown(rhi::Device& dev) override {
        for (auto& k : m_pipes)
            for (auto p : k)
                if (p) dev.destroy(p);
        dev.destroy(m_mask);
        for (rhi::TextureHandle t : {m_cascades, m_atlas, m_points}) {
            if (t) dev.destroy(t);
        }
    }

    void setup(FeatureContext& ctx) override {
        FrameState& fs = frameState(ctx);
        rhi::Device& dev = ctx.device();
        const RenderSettings& st = ctx.settings();
        const RenderSnapshot& snap = ctx.snapshot();
        GpuViewConstants& c = ctx.viewConstants();
        RenderView& view = ctx.view();
        rhi::RenderGraph& graph = ctx.graph();
        FrameResources& R = ctx.resources();
        ensureTextures(dev, st);

        std::unordered_map<u32, const SnapshotLight*> byEntity;
        for (const SnapshotLight& l : snap.lights) byEntity[l.entityId] = &l;
        auto castsShadows = [&](const GpuLight& g) {
            auto it = byEntity.find(g.entityId);
            return it != byEntity.end() && it->second->light.castShadows;
        };
        auto source = [&](const GpuLight& g) -> const LightComponent* {
            auto it = byEntity.find(g.entityId);
            return it != byEntity.end() ? &it->second->light : nullptr;
        };

        c.shadowParams = {st.csmBlend, st.shadowFilterRadius, st.pcss ? 1.0f : 0.0f, f32(st.pcfTaps)};
        c.shadowAtlasSize = u32(st.atlasSize);
        const VkDeviceAddress viewAddr = ctx.viewAddress(), sceneAddr = ctx.sceneAddress();

        // --- directional cascades ---
        const CameraParams& cam = view.camera();
        bool sunShadows = false;
        if (c.sunLight >= 0 && castsShadows(fs.lights[usize(c.sunLight)])) {
            const GpuLight& sun = fs.lights[usize(c.sunLight)];
            const LightComponent* lc = source(sun);
            CascadeInput in;
            in.cameraWorld = cam.world;
            in.verticalFov = cam.verticalFov;
            in.aspect = view.outputExtent().aspect();
            in.orthographic = cam.projection == CameraParams::Projection::Orthographic;
            in.orthographicHeight = cam.orthographicHeight;
            in.nearPlane = cam.nearPlane;
            in.shadowDistance = cam.farPlane > 0.0f ? std::min(st.csmDistance, cam.farPlane) : st.csmDistance;
            in.cascadeCount = u32(st.csmCascades);
            in.lambda = st.csmLambda;
            in.lightDirection = sun.direction;
            in.resolution = u32(st.csmResolution);
            const std::vector<Cascade> cascades = computeCascades(in);
            c.cascadeCount = u32(cascades.size());
            for (u32 i = 0; i < cascades.size(); ++i) {
                c.cascadeViewProj[i] = cascades[i].viewProj;
                c.cascadeSplits[i] = cascades[i].splitFar;
                c.cascadeTexelWorld[i] = cascades[i].texelWorld;
                c.cascadeDepthRange[i] = cascades[i].depthRange;
            }
            c.cascadeTexture = dev.sampledIndex(m_cascades);
            c.csmResolution = f32(st.csmResolution);
            c.sunBias = lc ? lc->shadowBias * 0.2f : 0.0f;
            c.sunNormalBias = lc ? std::clamp(lc->shadowNormalBias * 100.0f, 0.0f, 10.0f) : 1.5f;
            sunShadows = true;

            struct CascadeDraw {
                DrawList list;
                VkDeviceAddress matrix;
            };
            auto draws = std::make_shared<std::vector<CascadeDraw>>();
            for (const Cascade& cs : cascades) {
                DrawFilter f;
                f.frustum = Frustum::fromViewProj(cs.viewProj, true);
                f.requiredInstanceFlags = kInstanceCastShadows;
                f.bucketMask = (1u << u32(DrawBucket::Opaque)) | (1u << u32(DrawBucket::Masked));
                CascadeDraw d;
                d.list = ctx.buildDrawList(f);
                d.matrix = ctx.upload(std::span<const glm::mat4>(&cs.viewProj, 1));
                draws->push_back(std::move(d));
            }
            const rhi::RGTexture cascadesRG = importPersistent(graph, dev, m_cascades);
            graph.addPass("Shadow.Cascades")
                .write(cascadesRG, rhi::Access::DepthStencilWrite)
                .execute([this, &fs, draws, viewAddr, sceneAddr](rhi::PassContext& p) {
                    FeatureContext fc(fs, this, InjectionPoint::Shadows);
                    for (u32 i = 0; i < draws->size(); ++i) {
                        rhi::RenderingDesc rd;
                        rd.depth = rhi::DepthAttachment{m_cascades, 0, i, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                        VK_ATTACHMENT_STORE_OP_STORE, {0.0f, 0}};
                        p.cmd.beginRendering(rd);
                        p.cmd.setDepthBias(0.0f, 0.0f, -2.0f);
                        const CascadeDraw& d = (*draws)[i];
                        ShadowPush pc{viewAddr, sceneAddr, d.list.instanceIds, d.matrix, 0, 0};
                        const rhi::PipelineHandle pipes[4] = {m_pipes[0][0], m_pipes[0][1], m_pipes[0][0], m_pipes[0][1]};
                        fc.drawBatches(p.cmd, d.list, pipes, &pc, sizeof(pc));
                        p.cmd.endRendering();
                    }
                });
            R.setTexture(res::kShadowCascades, cascadesRG);
            ctx.stats().shadowMapsRendered += u32(cascades.size());
        }

        // --- local lights ---
        std::vector<ShadowRequest> requests;
        for (u32 i = fs.directionalCount; i < fs.lights.size(); ++i) {
            const GpuLight& g = fs.lights[i];
            if (!castsShadows(g)) continue;
            const LightComponent* lc = source(g);
            ShadowRequest r;
            r.lightIndex = i;
            r.position = g.position;
            r.range = g.range;
            r.point = g.type == u32(GpuLightType::Point);
            r.priority = 1.0f;
            r.resolutionHint = lc ? lc->shadowResolution : 0;
            requests.push_back(r);
        }
        ShadowBudget budget;
        budget.atlasSize = u32(st.atlasSize);
        budget.minResolution = u32(std::max(st.spotMinResolution, 16));
        budget.maxResolution = u32(std::max(st.spotMaxResolution, st.spotMinResolution));
        budget.maxShadowedLights = u32(std::max(st.maxShadowedLights, 0));
        budget.maxPointLights = std::min(u32(std::max(st.maxPointShadows, 0)), m_pointSlots);
        budget.pointResolution = u32(st.pointResolution);
        ShadowAtlasAllocator atlas(budget.atlasSize, budget.minResolution);
        const std::vector<ShadowAllocation> allocs =
            allocateShadows(requests, cam.position(), cam.verticalFov, f32(view.renderExtent().height), budget, atlas);

        const bool viewChanged = m_lastView != view.id();
        m_lastView = view.id();
        const auto& moved = ctx.scene().movedBounds();
        struct LocalDraw {
            DrawList list;
            VkDeviceAddress matrices;
            ShadowAtlasAllocator::Tile tile;
            u32 cubeSlot;
            bool point;
        };
        auto locals = std::make_shared<std::vector<LocalDraw>>();
        std::unordered_map<u32, CacheEntry> nextCache;
        u32 cached = 0;
        const f32 guard = st.shadowFilterRadius * 2.0f + 3.0f;
        for (const ShadowAllocation& a : allocs) {
            GpuLight& g = fs.lights[a.lightIndex];
            const LightComponent* lc = source(g);
            const f32 nearPlane = std::clamp(g.range * 0.004f, 0.02f, 0.5f);
            GpuShadow s;
            s.nearPlane = nearPlane;
            s.farPlane = g.range;
            s.normalBias = lc ? std::clamp(lc->shadowNormalBias * 100.0f, 0.0f, 10.0f) : 1.5f;
            s.bias = 0.0f;
            s.lightSize = g.sourceRadius;
            const f32 outer = lc ? lc->outerConeAngle : 45.0f;
            std::array<glm::mat4, 6> matrices{};
            u32 matrixCount = 1;
            if (a.point) {
                const f32 fov = cubeFaceFov(a.resolution, guard);
                const f32 tanHalf = std::tan(fov * 0.5f);
                s.kind = u32(GpuShadowKind::Point);
                s.cubeLayer = a.cubeSlot;
                s.atlasRect = {tanHalf, f32(a.resolution), 0.0f, 0.0f};
                s.texelSize = 2.0f * tanHalf / f32(a.resolution);
                for (u32 f = 0; f < 6; ++f) matrices[f] = cubeFaceViewProj(g.position, f, fov, nearPlane, g.range);
                matrixCount = 6;
            } else {
                s.kind = u32(GpuShadowKind::Spot);
                s.viewProj = spotViewProj(g.position, g.direction, outer, nearPlane, g.range);
                const f32 inv = 1.0f / f32(budget.atlasSize);
                s.atlasRect = {f32(a.tile.x) * inv, f32(a.tile.y) * inv, f32(a.tile.size) * inv, f32(a.tile.size) * inv};
                const f32 fov = std::min(glm::radians(outer) * 2.0f + glm::radians(2.0f), glm::radians(170.0f));
                s.texelSize = 2.0f * std::tan(fov * 0.5f) / f32(a.tile.size);
                matrices[0] = s.viewProj;
            }
            g.shadowIndex = i32(fs.shadows.size());
            fs.shadows.push_back(s);

            // Cache: re-render only when the allocation, the light or a moving caster inside its range changed.
            CacheEntry e;
            e.resolution = a.resolution;
            e.tile = a.tile;
            e.cubeSlot = a.point ? a.cubeSlot : ~0u;
            e.position = g.position;
            e.direction = g.direction;
            e.range = g.range;
            e.outer = outer;
            bool dirty = !st.shadowCaching || viewChanged || m_texturesRecreated;
            auto prev = m_cache.find(g.entityId);
            if (!dirty) {
                if (prev == m_cache.end()) dirty = true;
                else {
                    const CacheEntry& p = prev->second;
                    dirty = p.resolution != e.resolution || !(p.tile == e.tile) || p.cubeSlot != e.cubeSlot ||
                            p.position != e.position || p.direction != e.direction || p.range != e.range || p.outer != e.outer;
                }
            }
            if (!dirty) {
                const Sphere lightSphere{g.position, g.range};
                for (const Sphere& m : moved) {
                    if (lightSphere.intersects(m)) {
                        dirty = true;
                        break;
                    }
                }
            }
            nextCache[g.entityId] = e;
            if (!dirty) {
                ++cached;
                continue;
            }
            DrawFilter f;
            if (a.point) f.sphere = Sphere{g.position, g.range};
            else f.frustum = Frustum::fromViewProj(s.viewProj, true);
            f.requiredInstanceFlags = kInstanceCastShadows;
            f.bucketMask = (1u << u32(DrawBucket::Opaque)) | (1u << u32(DrawBucket::Masked));
            LocalDraw d;
            d.list = ctx.buildDrawList(f);
            d.matrices = ctx.upload(std::span<const glm::mat4>(matrices.data(), matrixCount));
            d.tile = a.tile;
            d.cubeSlot = a.cubeSlot;
            d.point = a.point;
            locals->push_back(std::move(d));
        }
        // Lights that lost their shadow: their old tiles become garbage; nothing to clear (tiles are cleared on reuse).
        m_cache = std::move(nextCache);
        m_texturesRecreated = false;
        ctx.stats().shadowedLights += u32(allocs.size());
        ctx.stats().shadowMapsCached += cached;
        for (const LocalDraw& d : *locals) ctx.stats().shadowMapsRendered += d.point ? 6 : 1;

        c.shadowAtlas = dev.sampledIndex(m_atlas);
        c.pointShadows = dev.sampledIndex(m_points);
        const rhi::RGTexture atlasRG = importPersistent(graph, dev, m_atlas);
        const rhi::RGTexture pointsRG = importPersistent(graph, dev, m_points);
        const bool anySpot = std::any_of(locals->begin(), locals->end(), [](const LocalDraw& d) { return !d.point; });
        const bool anyPoint = std::any_of(locals->begin(), locals->end(), [](const LocalDraw& d) { return d.point; });
        if (anySpot) {
            const u32 atlasSize = budget.atlasSize;
            graph.addPass("Shadow.Atlas")
                .write(atlasRG, rhi::Access::DepthStencilWrite)
                .execute([this, &fs, locals, viewAddr, sceneAddr, atlasSize](rhi::PassContext& p) {
                    FeatureContext fc(fs, this, InjectionPoint::Shadows);
                    rhi::RenderingDesc rd;
                    rd.depth = rhi::DepthAttachment{m_atlas, 0, 0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
                    p.cmd.beginRendering(rd);
                    for (const LocalDraw& d : *locals) {
                        if (d.point) continue;
                        p.cmd.setViewport(f32(d.tile.x), f32(d.tile.y), f32(d.tile.size), f32(d.tile.size));
                        p.cmd.setScissor(i32(d.tile.x), i32(d.tile.y), d.tile.size, d.tile.size);
                        VkClearAttachment clear{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
                        clear.clearValue.depthStencil = {0.0f, 0};
                        VkClearRect rect{{{i32(d.tile.x), i32(d.tile.y)}, {d.tile.size, d.tile.size}}, 0, 1};
                        vkCmdClearAttachments(p.cmd.vk(), 1, &clear, 1, &rect);
                        p.cmd.setDepthBias(0.0f, 0.0f, -1.5f);
                        ShadowPush pc{viewAddr, sceneAddr, d.list.instanceIds, d.matrices, 0, 0};
                        const rhi::PipelineHandle pipes[4] = {m_pipes[1][0], m_pipes[1][1], m_pipes[1][0], m_pipes[1][1]};
                        fc.drawBatches(p.cmd, d.list, pipes, &pc, sizeof(pc));
                    }
                    p.cmd.endRendering();
                    (void)atlasSize;
                });
        }
        if (anyPoint) {
            graph.addPass("Shadow.Points")
                .write(pointsRG, rhi::Access::DepthStencilWrite)
                .execute([this, &fs, locals, viewAddr, sceneAddr](rhi::PassContext& p) {
                    FeatureContext fc(fs, this, InjectionPoint::Shadows);
                    for (const LocalDraw& d : *locals) {
                        if (!d.point) continue;
                        if (m_layered) {
                            rhi::RenderingDesc rd;
                            rd.depth = rhi::DepthAttachment{m_points, 0, d.cubeSlot * 6, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                            VK_ATTACHMENT_STORE_OP_STORE, {0.0f, 0}};
                            rd.layerCount = 6;
                            p.cmd.beginRendering(rd);
                            p.cmd.setDepthBias(0.0f, 0.0f, -1.5f);
                            ShadowPush pc{viewAddr, sceneAddr, d.list.instanceIds, d.matrices, 0, 0};
                            const rhi::PipelineHandle pipes[4] = {m_pipes[2][0], m_pipes[2][1], m_pipes[2][0], m_pipes[2][1]};
                            fc.drawBatches(p.cmd, d.list, pipes, &pc, sizeof(pc), 6);
                            p.cmd.endRendering();
                        } else {
                            for (u32 face = 0; face < 6; ++face) {
                                rhi::RenderingDesc rd;
                                rd.depth = rhi::DepthAttachment{m_points, 0, d.cubeSlot * 6 + face, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                                VK_ATTACHMENT_STORE_OP_STORE, {0.0f, 0}};
                                p.cmd.beginRendering(rd);
                                p.cmd.setDepthBias(0.0f, 0.0f, -1.5f);
                                ShadowPush pc{viewAddr, sceneAddr, d.list.instanceIds, d.matrices + face * sizeof(glm::mat4), 0, 0};
                                const rhi::PipelineHandle pipes[4] = {m_pipes[1][0], m_pipes[1][1], m_pipes[1][0], m_pipes[1][1]};
                                fc.drawBatches(p.cmd, d.list, pipes, &pc, sizeof(pc));
                                p.cmd.endRendering();
                            }
                        }
                    }
                });
        }
        if (!allocs.empty() || anySpot || anyPoint) {
            R.setTexture(res::kShadowAtlas, atlasRG);
            R.setTexture(res::kPointShadows, pointsRG);
        }

        // --- screen-space sun visibility ---
        if (sunShadows) {
            const rhi::RGTexture depth = R.texture(res::kDepth), normals = R.texture(res::kNormals);
            const rhi::RGTexture cascadesRG = R.texture(res::kShadowCascades);
            rhi::TextureDesc md;
            md.format = formats::kShadowMask;
            md.width = view.renderExtent().width;
            md.height = view.renderExtent().height;
            md.usage = rhi::TextureUsage::None;
            md.name = "ShadowMask";
            const rhi::RGTexture mask = graph.createTexture(md);
            graph.addPass("ShadowMask")
                .read(depth, rhi::Access::SampledFragment)
                .read(normals, rhi::Access::SampledFragment)
                .read(cascadesRG, rhi::Access::SampledFragment)
                .color(mask, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
                .execute([this, depth, normals, viewAddr, sceneAddr](rhi::PassContext& p) {
                    struct {
                        u64 view, scene;
                        u32 depth, normals;
                    } pc{viewAddr, sceneAddr, p.sampledIndex(depth), p.sampledIndex(normals)};
                    drawFullscreen(p.cmd, m_mask, &pc, sizeof(pc));
                });
            R.setTexture(res::kShadowMask, mask);
        }
    }

private:
    void ensureTextures(rhi::Device& dev, const RenderSettings& st) {
        auto make = [&](rhi::TextureHandle& t, u32 size, u32 layers, const char* name) {
            if (t) {
                const rhi::TextureDesc& d = dev.desc(t);
                if (d.width == size && d.arrayLayers == layers) return;
                dev.destroy(t);
            }
            rhi::TextureDesc d;
            d.name = name;
            d.format = formats::kShadowDepth;
            d.width = d.height = size;
            d.arrayLayers = layers;
            d.usage = rhi::TextureUsage::DepthStencilAttachment | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
            t = dev.createTexture(d);
            // Start "fully lit" so cached tiles that were never rendered read as unshadowed.
            dev.immediateSubmit([&](rhi::CommandList& cmd) {
                cmd.transition(t, rhi::Access::TransferWrite, true);
                cmd.clearDepthStencil(t, {0.0f, 0});
                cmd.transition(t, rhi::Access::SampledFragment);
            });
            m_texturesRecreated = true;
        };
        const u32 cascades = u32(std::clamp(st.csmCascades, 1, 4));
        make(m_cascades, u32(st.csmResolution), std::max(cascades, 2u), "render.shadowCascades");
        make(m_atlas, u32(st.atlasSize), 1, "render.shadowAtlas");
        m_pointSlots = u32(std::clamp(st.maxPointShadows, 1, 64));
        make(m_points, u32(st.pointResolution), m_pointSlots * 6, "render.pointShadows");
    }

    rhi::PipelineHandle m_pipes[3][2];
    rhi::PipelineHandle m_mask;
    rhi::TextureHandle m_cascades, m_atlas, m_points;
    u32 m_pointSlots = 1;
    bool m_layered = false;
    bool m_texturesRecreated = true;
    ViewId m_lastView = 0;
    std::unordered_map<u32, CacheEntry> m_cache;
};

} // namespace

std::unique_ptr<IRenderFeature> makeShadowsRasterFeature() { return std::make_unique<ShadowsRasterFeature>(); }

} // namespace ox::render
