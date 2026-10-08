// CDLOD terrain renderer: heightmap (R32F / R16) + normal map (compute, mips) + hole mask + 2 splat textures per
// terrain, updated with dirty-rect copies; quadtree selection per view / cascade on the CPU; instanced grid draws.
#include "world_geometry.hpp"

#if OX_RENDER_HAS_WORLD

#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/render/render_stats.hpp>
#include <oxwald/rhi/vulkan.hpp>

#include <cstring>

namespace ox::render::worldfx {

namespace {

constexpr u32 kFlagHoles = 1u, kFlagSplat = 2u, kFlagTriplanar = 4u;

struct TerrainGpuParams {
    glm::vec4 originSize;
    glm::vec4 heightParams;
    glm::vec4 morphCamera;
    glm::vec4 splatRect;
    glm::vec4 shading0;
    glm::vec4 shading1;
    glm::vec4 skirt[4];
    glm::vec4 tess; // range, max factor, height, enabled
    u32 heightTex, normalTex, holesTex, splat0, splat1, flags, entityId, pad;
    u32 layerMaterial[8];
};
static_assert(sizeof(TerrainGpuParams) == 240);
constexpr f32 kTessRange = 40.0f, kTessMaxFactor = 8.0f;

struct TerrainPush {
    u64 view = 0, scene = 0, params = 0, patches = 0, grid = 0, matrices = 0;
    u32 inputs[4] = {kInvalidIndex, kInvalidIndex, kInvalidIndex, kInvalidIndex};
};
static_assert(sizeof(TerrainPush) == 64);

world::IRect clampRect(world::IRect r, u32 res) {
    return r.intersected({0, 0, i32(res), i32(res)});
}

// Copies the rows of `rect` out of a row-major sample array.
std::vector<u8> extractRows(std::span<const u8> src, u32 rowSamples, usize bytesPerSample, const world::IRect& rect) {
    std::vector<u8> out(usize(rect.width()) * rect.height() * bytesPerSample);
    for (i32 z = rect.z0; z < rect.z1; ++z) {
        std::memcpy(out.data() + usize(z - rect.z0) * rect.width() * bytesPerSample,
                    src.data() + (usize(z) * rowSamples + rect.x0) * bytesPerSample, usize(rect.width()) * bytesPerSample);
    }
    return out;
}

void copyRegion(rhi::Device& dev, rhi::CommandList& cmd, rhi::BufferHandle staging, u64 offset, rhi::TextureHandle tex,
                const world::IRect& r) {
    VkBufferImageCopy c{};
    c.bufferOffset = offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {r.x0, r.z0, 0};
    c.imageExtent = {u32(r.width()), u32(r.height()), 1};
    vkCmdCopyBufferToImage(cmd.vk(), dev.vkBuffer(staging), dev.vkImage(tex), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

rhi::TextureHandle makeTexture(rhi::Device& dev, VkFormat format, u32 size, u32 mips, rhi::TextureUsage usage, std::string name) {
    rhi::TextureDesc d;
    d.format = format;
    d.width = d.height = size;
    d.mipLevels = mips;
    d.usage = usage;
    d.name = std::move(name);
    return dev.createTexture(d);
}

} // namespace

bool TerrainSystem::initialize(rhi::Device& dev) {
    m_normals = createComputePipeline(dev, "world.terrain.normals", "render/world/terrain_normals.comp");
    for (u32 entity = 0; entity < 2; ++entity) {
        rhi::GraphicsPipelineDesc d;
        d.name = entity ? "world.terrain.prepass.id" : "world.terrain.prepass";
        d.vertex = rhi::ShaderStageDesc::file("render/world/terrain.vert");
        std::vector<rhi::ShaderDefine> defs;
        if (entity) defs.push_back({"OX_ENTITY_ID"});
        d.fragment = rhi::ShaderStageDesc::file("render/world/terrain_prepass.frag", defs);
        d.colorFormats = {formats::kNormals, formats::kVelocity};
        if (entity) {
            d.colorFormats.push_back(formats::kEntityId);
            rhi::BlendState noBlend;
            noBlend.srcColor = noBlend.srcAlpha = VK_BLEND_FACTOR_ONE;
            noBlend.dstColor = noBlend.dstAlpha = VK_BLEND_FACTOR_ZERO;
            noBlend.writeMask = VK_COLOR_COMPONENT_R_BIT;
            d.blend = {rhi::BlendState::opaque(), rhi::BlendState::opaque(), noBlend};
        }
        d.depthFormat = formats::kDepth;
        d.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
        d.raster.cullMode = VK_CULL_MODE_BACK_BIT;
        d.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        pipePrepass[entity] = dev.createGraphicsPipeline(d);
    }
    {
        rhi::GraphicsPipelineDesc d;
        d.name = "world.terrain.forward";
        d.vertex = rhi::ShaderStageDesc::file("render/world/terrain.vert");
        d.fragment = rhi::ShaderStageDesc::file("render/world/terrain.frag");
        d.colorFormats = {formats::kSceneColor};
        d.depthFormat = formats::kDepth;
        d.depth = {true, false, VK_COMPARE_OP_EQUAL};
        d.raster.cullMode = VK_CULL_MODE_BACK_BIT;
        d.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        pipeForward = dev.createGraphicsPipeline(d);
    }
    if (dev.caps().tessellationShader) {
        // Same vertex + tessellation stages for the prepass and the forward pass (depth EQUAL invariance).
        for (u32 v = 0; v < 3; ++v) {
            rhi::GraphicsPipelineDesc d;
            d.name = v == 2 ? "world.terrain.tess.forward" : v ? "world.terrain.tess.prepass.id" : "world.terrain.tess.prepass";
            d.vertex = rhi::ShaderStageDesc::file("render/world/terrain.vert");
            d.tessControl = rhi::ShaderStageDesc::file("render/world/terrain.tesc");
            d.tessEval = rhi::ShaderStageDesc::file("render/world/terrain.tese");
            d.patchControlPoints = 3;
            d.depthFormat = formats::kDepth;
            d.raster.cullMode = VK_CULL_MODE_BACK_BIT;
            d.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            if (v == 2) {
                d.fragment = rhi::ShaderStageDesc::file("render/world/terrain.frag");
                d.colorFormats = {formats::kSceneColor};
                d.depth = {true, false, VK_COMPARE_OP_EQUAL};
                tessForward = dev.createGraphicsPipeline(d);
            } else {
                std::vector<rhi::ShaderDefine> defs;
                if (v) defs.push_back({"OX_ENTITY_ID"});
                d.fragment = rhi::ShaderStageDesc::file("render/world/terrain_prepass.frag", defs);
                d.colorFormats = {formats::kNormals, formats::kVelocity};
                if (v) {
                    d.colorFormats.push_back(formats::kEntityId);
                    rhi::BlendState noBlend;
                    noBlend.srcColor = noBlend.srcAlpha = VK_BLEND_FACTOR_ONE;
                    noBlend.dstColor = noBlend.dstAlpha = VK_BLEND_FACTOR_ZERO;
                    noBlend.writeMask = VK_COLOR_COMPONENT_R_BIT;
                    d.blend = {rhi::BlendState::opaque(), rhi::BlendState::opaque(), noBlend};
                }
                d.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
                tessPrepass[v] = dev.createGraphicsPipeline(d);
            }
        }
    }
    {
        rhi::GraphicsPipelineDesc d;
        d.name = "world.terrain.shadow";
        d.vertex = rhi::ShaderStageDesc::file("render/world/terrain.vert", {{"OX_TERRAIN_SHADOW"}});
        d.fragment = rhi::ShaderStageDesc::file("render/world/terrain_prepass.frag", {{"OX_TERRAIN_SHADOW"}});
        d.depthFormat = formats::kShadowDepth;
        d.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
        d.raster.cullMode = VK_CULL_MODE_NONE;
        d.raster.depthBias = true;
        d.raster.depthClamp = dev.caps().depthClamp;
        pipeShadow = dev.createGraphicsPipeline(d);
    }
    return true;
}

void TerrainSystem::shutdown(rhi::Device& dev) {
    for (auto& [id, t] : m_terrains) destroy(dev, t);
    m_terrains.clear();
    for (auto& [dim, g] : m_grids) {
        dev.destroy(g.vertices);
        dev.destroy(g.indices);
    }
    m_grids.clear();
    for (rhi::PipelineHandle p : {m_normals, pipePrepass[0], pipePrepass[1], pipeForward, pipeShadow, tessPrepass[0],
                                  tessPrepass[1], tessForward}) {
        if (p) dev.destroy(p);
    }
}

void TerrainSystem::destroy(rhi::Device& dev, TerrainGpu& t) {
    for (rhi::TextureHandle h : {t.height, t.normal, t.holes, t.splat0, t.splat1}) {
        if (h) dev.destroy(h);
    }
    t.height = t.normal = t.holes = t.splat0 = t.splat1 = {};
}

TerrainSystem::GridGpu& TerrainSystem::grid(rhi::Device& dev, u32 gridDim) {
    auto it = m_grids.find(gridDim);
    if (it != m_grids.end()) return it->second;
    const world::TerrainGridMesh mesh = world::generateTerrainGrid(gridDim, true);
    GridGpu g;
    g.gridDim = gridDim;
    g.indexCount = u32(mesh.indices.size());
    g.quadrants = mesh.quadrants;
    static_assert(sizeof(world::TerrainGridVertex) == 12);
    g.vertices = dev.createBuffer({mesh.vertices.size() * sizeof(world::TerrainGridVertex), rhi::BufferUsage::Storage,
                                   rhi::MemoryUsage::GpuOnly, "world.terrain.gridVertices"},
                                  mesh.vertices.data());
    g.indices = dev.createBuffer({mesh.indices.size() * 4, rhi::BufferUsage::Index, rhi::MemoryUsage::GpuOnly,
                                  "world.terrain.gridIndices"},
                                 mesh.indices.data());
    return m_grids.emplace(gridDim, g).first->second;
}

void TerrainSystem::uploadHeight(rhi::Device& dev, TerrainGpu& t, const TerrainSnapshot& s) {
    OX_PROFILE_ZONE();
    const world::Heightfield& hf = *s.heightfield;
    const world::HeightfieldDesc& d = hf.desc();
    const u32 res = d.resolution;
    const bool hasHoles = hf.hasHoles();
    const bool recreate = !t.height || t.desc.resolution != res || t.desc.format != d.format || t.hasHoles != hasHoles;
    const bool full = recreate || s.fullUpload || t.heightVersion != s.dirtySinceVersion;
    world::IRect rect = full ? hf.fullRect() : clampRect(s.dirtyRect, res);
    if (!full && rect.empty()) {
        t.heightVersion = s.heightfieldVersion;
        t.heightfield = s.heightfield;
        return;
    }
    if (recreate) {
        destroy(dev, t);
        using U = rhi::TextureUsage;
        const VkFormat hfmt = d.format == world::HeightFormat::Float32 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R16_UNORM;
        t.height = makeTexture(dev, hfmt, res, 1, U::Sampled | U::TransferDst, "world.terrain.height");
        t.normal = makeTexture(dev, VK_FORMAT_R8G8B8A8_UNORM, res, rhi::fullMipCount(res, res),
                               U::Sampled | U::Storage | U::TransferSrc | U::TransferDst, "world.terrain.normals");
        if (hasHoles) t.holes = makeTexture(dev, VK_FORMAT_R8_UNORM, res, 1, U::Sampled | U::TransferDst, "world.terrain.holes");
        t.hasHoles = hasHoles;
        t.splatVersion = ~0ull; // splat textures were destroyed too
    }
    t.desc = d;
    const usize bps = hf.bytesPerSample();
    std::vector<u8> heights = extractRows(hf.rawBytes(), res, bps, rect);
    std::vector<u8> holes;
    if (hasHoles) {
        holes = extractRows(hf.holeMask(), res, 1, rect);
        for (u8& h : holes) h = h ? 255 : 0;
    }
    const u64 holesOffset = (heights.size() + 15) & ~u64(15);
    rhi::BufferHandle staging = dev.createBuffer(
        {holesOffset + std::max<u64>(holes.size(), 4), rhi::BufferUsage::TransferSrc, rhi::MemoryUsage::Upload, "world.terrain.staging"});
    std::memcpy(dev.mapped(staging), heights.data(), heights.size());
    if (!holes.empty()) std::memcpy(static_cast<u8*>(dev.mapped(staging)) + holesOffset, holes.data(), holes.size());
    // Normals depend on the neighbours of every changed sample.
    const world::IRect nrect = clampRect({rect.x0 - 1, rect.z0 - 1, rect.x1 + 1, rect.z1 + 1}, res);
    const u32 heightIdx = dev.sampledIndex(t.height);
    const u32 normalStorage = dev.storageIndex(t.normal, 0);
    dev.immediateSubmit([&](rhi::CommandList& cmd) {
        cmd.transition(t.height, rhi::Access::TransferWrite, full);
        copyRegion(dev, cmd, staging, 0, t.height, rect);
        if (t.holes) {
            cmd.transition(t.holes, rhi::Access::TransferWrite, full);
            copyRegion(dev, cmd, staging, holesOffset, t.holes, rect);
            cmd.transition(t.holes, rhi::Access::SampledGraphics);
        }
        cmd.transition(t.height, rhi::Access::SampledCompute);
        cmd.transition(t.normal, rhi::Access::StorageWriteCompute, full);
        cmd.bindPipeline(m_normals);
        struct {
            u32 height, dst;
            i32 rectMin[2], rectSize[2];
            i32 resolution;
            f32 heightScale, spacing;
        } pc{heightIdx, normalStorage, {nrect.x0, nrect.z0}, {nrect.width(), nrect.height()}, i32(res), d.heightScale,
             hf.spacing()};
        cmd.pushConstants(pc);
        cmd.dispatch((u32(nrect.width()) + 7) / 8, (u32(nrect.height()) + 7) / 8);
        cmd.generateMipmaps(t.normal, rhi::Access::SampledGraphics);
        cmd.transition(t.height, rhi::Access::SampledGraphics);
    });
    dev.destroy(staging);
    t.heightVersion = s.heightfieldVersion;
    t.heightfield = s.heightfield;
}

void TerrainSystem::uploadSplat(rhi::Device& dev, TerrainGpu& t, const TerrainSnapshot& s) {
    OX_PROFILE_ZONE();
    const world::SplatMap& sm = *s.splat;
    const u32 res = sm.resolution();
    const u32 textures = sm.layerCount() > 4 ? 2 : 1;
    const bool recreate = !t.splat0 || t.splatResolution != res || (textures == 2) != bool(t.splat1);
    const bool full = recreate || s.splatFullUpload || t.splatVersion != s.splatDirtySinceVersion;
    const world::IRect rect = full ? sm.fullRect() : clampRect(s.splatDirtyRect, res);
    t.splatLayers = sm.layerCount();
    t.splatOrigin = sm.origin();
    t.splatSize = sm.worldSize();
    if (!full && rect.empty()) {
        t.splatVersion = s.splatVersion;
        return;
    }
    if (recreate) {
        for (rhi::TextureHandle* h : {&t.splat0, &t.splat1}) {
            if (*h) dev.destroy(*h);
            *h = {};
        }
        using U = rhi::TextureUsage;
        t.splat0 = makeTexture(dev, VK_FORMAT_R8G8B8A8_UNORM, res, 1, U::Sampled | U::TransferDst, "world.terrain.splat0");
        if (textures == 2) t.splat1 = makeTexture(dev, VK_FORMAT_R8G8B8A8_UNORM, res, 1, U::Sampled | U::TransferDst, "world.terrain.splat1");
        t.splatResolution = res;
    }
    std::vector<u8> d0 = sm.packRgba8(0, rect);
    std::vector<u8> d1 = textures == 2 ? sm.packRgba8(1, rect) : std::vector<u8>{};
    const u64 off1 = (d0.size() + 15) & ~u64(15);
    rhi::BufferHandle staging = dev.createBuffer(
        {off1 + std::max<u64>(d1.size(), 4), rhi::BufferUsage::TransferSrc, rhi::MemoryUsage::Upload, "world.terrain.splatStaging"});
    std::memcpy(dev.mapped(staging), d0.data(), d0.size());
    if (!d1.empty()) std::memcpy(static_cast<u8*>(dev.mapped(staging)) + off1, d1.data(), d1.size());
    dev.immediateSubmit([&](rhi::CommandList& cmd) {
        cmd.transition(t.splat0, rhi::Access::TransferWrite, full);
        copyRegion(dev, cmd, staging, 0, t.splat0, rect);
        cmd.transition(t.splat0, rhi::Access::SampledGraphics);
        if (t.splat1) {
            cmd.transition(t.splat1, rhi::Access::TransferWrite, full);
            copyRegion(dev, cmd, staging, off1, t.splat1, rect);
            cmd.transition(t.splat1, rhi::Access::SampledGraphics);
        }
    });
    dev.destroy(staging);
    t.splatVersion = s.splatVersion;
}

void TerrainSystem::update(FeatureContext& ctx, const WorldSnapshot* world, const WorldCVars& cv) {
    OX_PROFILE_ZONE();
    rhi::Device& dev = ctx.device();
    ++m_frame;
    if (world) {
        for (const TerrainSnapshot& s : world->terrains) {
            if (!s.heightfield || !s.heightfield->valid()) continue;
            TerrainGpu& t = m_terrains[s.entityId];
            t.entityId = s.entityId;
            t.lastSeen = m_frame;
            t.layerMaterials = s.layerMaterials;
            t.settings = s.settings;
            const bool heightChanged = t.heightVersion != s.heightfieldVersion || !t.height;
            const u64 prevVersion = t.heightVersion;
            const world::HeightfieldDesc prevDesc = t.desc;
            if (heightChanged) uploadHeight(dev, t, s);
            if (s.splat && s.splat->resolution() >= 2) {
                if (t.splatVersion != s.splatVersion || !t.splat0) uploadSplat(dev, t, s);
            } else if (t.splat0) {
                dev.destroy(t.splat0);
                if (t.splat1) dev.destroy(t.splat1);
                t.splat0 = t.splat1 = {};
                t.splatLayers = 0;
            }
            // CDLOD quadtree (render-side selection, r.Terrain.LODScale scales every range).
            world::TerrainLodSettings lod = s.lod;
            lod.viewDistance *= std::max(cv.terrainLodScale, 0.05f);
            const bool lodChanged = !t.quadtree || std::memcmp(&lod, &t.lodBuilt, sizeof(lod)) != 0 ||
                                    prevDesc.resolution != t.desc.resolution || prevDesc.worldSize != t.desc.worldSize ||
                                    prevDesc.origin != t.desc.origin || prevDesc.heightScale != t.desc.heightScale ||
                                    prevDesc.heightOffset != t.desc.heightOffset;
            if (lodChanged || (heightChanged && (s.fullUpload || prevVersion != s.dirtySinceVersion))) {
                t.quadtree = std::make_unique<world::TerrainQuadtree>(*s.heightfield, lod);
                t.lodBuilt = lod;
                t.skirt.clear();
                for (u32 l = 0; l < lod.lodCount; ++l) t.skirt.push_back(t.quadtree->skirtDepth(l));
            } else if (heightChanged && !s.dirtyRect.empty()) {
                t.quadtree->updateBounds(*s.heightfield, s.dirtyRect);
            }
        }
    }
    for (auto it = m_terrains.begin(); it != m_terrains.end();) {
        if (it->second.lastSeen != m_frame) {
            destroy(dev, it->second);
            it = m_terrains.erase(it);
        } else {
            ++it;
        }
    }
}

std::shared_ptr<TerrainSystem::ViewData> TerrainSystem::prepare(FeatureContext& ctx, const glm::vec3& lodCamera,
                                                                const glm::mat4& cullViewProj, bool shadow,
                                                                const WorldCVars& cv) {
    if (m_terrains.empty()) return nullptr;
    OX_PROFILE_ZONE();
    rhi::Device& dev = ctx.device();
    GpuResourceCache& cache = ctx.renderer().resources();
    auto out = std::make_shared<ViewData>();
    world::TerrainSelection sel;
    const world::Frustum frustum = world::Frustum::fromViewProjection(cullViewProj);
    for (auto& [id, t] : m_terrains) {
        if (!t.quadtree || !t.height) continue;
        if (shadow && !t.settings.castShadows) continue;
        sel.clear();
        t.quadtree->select({.cameraPosition = lodCamera, .frustum = frustum, .maxPatches = 16384}, sel);
        if (sel.patches.empty()) continue;
        GridGpu& g = grid(dev, t.lodBuilt.leafNodeSize);
        // LOD-0 patches near the camera go through the tessellation pipelines (Ultra, when supported).
        const bool tess = !shadow && cv.terrainTessellation && tessForward && t.settings.tessellationHeight > 0.0f;
        auto tessellate = [&](const world::TerrainPatch& p) {
            if (!tess || p.lod != 0) return false;
            const glm::vec2 cam(lodCamera.x, lodCamera.z);
            return glm::distance(glm::clamp(cam, p.offset, p.offset + glm::vec2(p.size)), cam) < kTessRange;
        };
        std::vector<Draw> groups;
        for (u32 group = 0; group < (tess ? 2u : 1u); ++group) {
            // Whole nodes first, then the partial nodes grouped per quadrant.
            std::vector<world::TerrainPatchGpu> patches;
            Draw d;
            d.tessellated = group == 1;
            auto mine = [&](const world::TerrainPatch& p) { return tessellate(p) == d.tessellated; };
            for (const world::TerrainPatch& p : sel.patches) {
                if (p.quadrantMask == 0xF && mine(p)) patches.push_back(world::toGpu(p));
            }
            d.fullCount = u32(patches.size());
            for (u32 q = 0; q < 4; ++q) {
                d.quadrantFirstInstance[q] = u32(patches.size());
                for (const world::TerrainPatch& p : sel.patches) {
                    if (p.quadrantMask != 0xF && (p.quadrantMask & (1u << q)) && mine(p)) patches.push_back(world::toGpu(p));
                }
                d.quadrantCount[q] = u32(patches.size()) - d.quadrantFirstInstance[q];
            }
            if (patches.empty()) continue;
            d.patches = ctx.upload(std::span<const world::TerrainPatchGpu>(patches));
            d.grid = dev.address(g.vertices);
            d.indexBuffer = g.indices;
            d.fullIndexCount = g.indexCount;
            d.quadrants = g.quadrants;
            d.triangles = u64(d.fullCount) * g.indexCount / 3;
            for (u32 q = 0; q < 4; ++q) d.triangles += u64(d.quadrantCount[q]) * g.quadrants[q].count / 3;
            groups.push_back(d);
        }

        TerrainGpuParams p{};
        const world::HeightfieldDesc& hd = t.desc;
        p.originSize = {hd.origin.x, hd.origin.y, hd.worldSize, hd.worldSize / f32(hd.resolution - 1)};
        p.heightParams = {hd.heightScale, hd.heightOffset, f32(hd.resolution), 1.0f / f32(hd.resolution)};
        p.morphCamera = glm::vec4(lodCamera, f32(g.gridDim));
        p.splatRect = {t.splatOrigin.x, t.splatOrigin.y, t.splatSize, f32(t.splatResolution)};
        const TerrainRenderComponent& rs = t.settings;
        p.shading0 = {std::cos(glm::radians(std::clamp(rs.triplanarSlopeDeg, 1.0f, 89.0f))), std::max(rs.heightBlend, 0.0f),
                      std::max(rs.macroVariation, 0.0f), std::clamp(rs.tilingBreakup, 0.0f, 1.0f)};
        const u32 layers = t.splat0 ? std::min<u32>(u32(t.layerMaterials.size()), std::min(t.splatLayers, 8u)) : 0u;
        p.shading1 = {1.0f / std::max(rs.layerTileMeters, 0.01f), f32(std::clamp(cv.terrainMaxLayers, 1, 8)), f32(layers),
                      rs.tessellationHeight};
        for (u32 l = 0; l < 16; ++l) p.skirt[l / 4][l % 4] = l < t.skirt.size() ? t.skirt[l] : (t.skirt.empty() ? 0.0f : t.skirt.back());
        p.heightTex = dev.sampledIndex(t.height);
        p.normalTex = dev.sampledIndex(t.normal);
        p.holesTex = t.holes ? dev.sampledIndex(t.holes) : kInvalidIndex;
        p.splat0 = t.splat0 ? dev.sampledIndex(t.splat0) : kInvalidIndex;
        p.splat1 = t.splat1 ? dev.sampledIndex(t.splat1) : p.splat0;
        p.flags = (t.hasHoles ? kFlagHoles : 0u) | (layers > 0 ? kFlagSplat : 0u) | (cv.terrainTriplanar ? kFlagTriplanar : 0u);
        p.entityId = t.entityId;
        p.tess = {kTessRange, kTessMaxFactor, t.settings.tessellationHeight, tess ? 1.0f : 0.0f};
        for (u32 l = 0; l < 8; ++l) {
            p.layerMaterial[l] = l < layers && t.layerMaterials[l].isValid() ? cache.materialIndex(t.layerMaterials[l]) : 0u;
        }
        const VkDeviceAddress params = ctx.upload(std::span<const TerrainGpuParams>(&p, 1));
        for (Draw& d : groups) {
            d.params = params;
            out->draws.push_back(d);
        }
    }
    if (out->draws.empty()) return nullptr;
    return out;
}

void TerrainSystem::draw(rhi::CommandList& cmd, const ViewData& data, WorldPass pass, const WorldPassContext& wc,
                         FeatureContext& fc) const {
    for (const Draw& d : data.draws) {
        const bool tess = d.tessellated && pass != WorldPass::Shadow;
        const u32 e = wc.entityIds ? 1u : 0u;
        const rhi::PipelineHandle pipe = pass == WorldPass::Prepass  ? (tess ? tessPrepass[e] : pipePrepass[e])
                                         : pass == WorldPass::Forward ? (tess ? tessForward : pipeForward)
                                                                      : pipeShadow;
        cmd.bindPipeline(pipe);
        TerrainPush pc;
        pc.view = wc.view;
        pc.scene = wc.scene;
        pc.params = d.params;
        pc.patches = d.patches;
        pc.grid = d.grid;
        pc.matrices = wc.matrix;
        for (u32 i = 0; i < 4; ++i) pc.inputs[i] = wc.inputs[i];
        cmd.pushConstants(pc);
        cmd.bindIndexBuffer(d.indexBuffer);
        if (d.fullCount) cmd.drawIndexed(d.fullIndexCount, d.fullCount, 0, 0, 0);
        for (u32 q = 0; q < 4; ++q) {
            if (d.quadrantCount[q]) {
                cmd.drawIndexed(d.quadrants[q].count, d.quadrantCount[q], d.quadrants[q].first, 0, d.quadrantFirstInstance[q]);
            }
        }
        u32 draws = (d.fullCount ? 1u : 0u);
        for (u32 q = 0; q < 4; ++q) draws += d.quadrantCount[q] ? 1u : 0u;
        for (u32 i = 0; i < draws; ++i) fc.countDraw(i == 0 ? d.triangles : 0, i == 0 ? d.fullCount : 0);
    }
}

} // namespace ox::render::worldfx

#endif // OX_RENDER_HAS_WORLD
