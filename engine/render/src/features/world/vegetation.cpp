// Vegetation renderer: instance arena (world::VegetationInstanceGpu, uploaded per batch version), CPU cell pre-cull,
// GPU per-instance frustum/distance/LOD culling into (prototype, LOD) slots, indirect draws (one per submesh and LOD;
// MoltenVK has no drawIndirectCount), dithered LOD cross-fade, octahedral impostors baked in-frame at load.
#include "world_geometry.hpp"

#if OX_RENDER_HAS_WORLD

#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/render/render_stats.hpp>

#include <bit>
#include <cstring>

namespace ox::render::worldfx {

namespace {

constexpr u32 kImpostorFrames = 8;
constexpr u32 kImpostorFrameSize = 128;
constexpr u32 kLods = 4; // mesh 0..2 + impostor

struct CullCellGpu {
    u32 first, count, layerSlot, pad;
};
struct CullLayerGpu {
    glm::vec4 lodEnds;
    glm::vec4 params;
    u32 protoSlot, pad0, pad1, pad2;
};
static_assert(sizeof(CullLayerGpu) == 48);
struct SlotGpu {
    u32 base, capacity, count, pad;
};
struct VegFrameGpu {
    glm::vec4 wind[2];
    glm::vec4 windPrev[2];
    glm::vec4 interactors[8];
    u32 interactorCount;
    f32 time, prevTime;
    u32 hasWind;
};
static_assert(sizeof(VegFrameGpu) == 208);
struct VegProtoGpu {
    glm::vec4 wind;
    glm::vec4 impostor;
    u32 albedoAtlas, normalAtlas, kind, pad;
};
static_assert(sizeof(VegProtoGpu) == 48);
struct VegPush {
    u64 view = 0, scene = 0, instances = 0, records = 0, frame = 0, protos = 0, matrices = 0;
    u32 proto = 0, material = 0, entityId = 0, lightDirOct = 0;
    u32 inputs[4] = {kInvalidIndex, kInvalidIndex, kInvalidIndex, kInvalidIndex};
};
static_assert(sizeof(VegPush) == 88);
struct CullPush {
    u64 cells, layers, instances, planes, slots, records;
    glm::vec3 cameraPos;
    u32 cellCount;
};
static_assert(sizeof(CullPush) == 64);
struct ArgsPush {
    u64 slots, args, commandSlots;
    u32 commandCount, pad;
};

// Mirrors oxHemiOctDecode / oxImpostorBasis (vegetation_common.glsl).
glm::vec3 hemiOctDecode(glm::vec2 e) {
    const glm::vec2 t = glm::vec2(e.x + e.y, e.x - e.y) * 0.5f;
    return glm::normalize(glm::vec3(t.x, 1.0f - std::abs(t.x) - std::abs(t.y), t.y));
}
void impostorBasis(glm::vec3 d, glm::vec3& right, glm::vec3& up) {
    const glm::vec3 refUp = std::abs(d.y) > 0.999f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    right = glm::normalize(glm::cross(refUp, d));
    up = glm::cross(d, right);
}

u32 lodAt(const world::VegetationLodSettings& s, bool impostors, f32 d) {
    if (d > s.cullDistance) return 4;
    const f32 ends[3] = {s.lodDistances[0], s.lodDistances[1], impostors ? s.impostorDistance : s.cullDistance};
    for (u32 i = 0; i < 3; ++i) {
        if (d <= ends[i]) return i;
    }
    return impostors ? 3u : 2u;
}

u64 bucket(u64 n, u64 minimum) { return std::bit_ceil(std::max(n, minimum)); }

} // namespace

bool VegetationSystem::initialize(rhi::Device& dev, Renderer& renderer) {
    m_device = &dev;
    m_renderer = &renderer;
    m_arena.init(dev, "world.vegetation.instances", sizeof(world::VegetationInstanceGpu), 16 * 1024, rhi::BufferUsage::Storage);
    const u32 quad[6] = {0, 1, 2, 0, 2, 3};
    m_quadIndices = dev.createBuffer({sizeof(quad), rhi::BufferUsage::Index, rhi::MemoryUsage::GpuOnly, "world.impostorQuad"}, quad);
    m_cull = createComputePipeline(dev, "world.vegetation.cull", "render/world/veg_cull.comp");
    m_args = createComputePipeline(dev, "world.vegetation.args", "render/world/veg_args.comp");

    auto graphics = [&](std::string name, const char* vert, const char* frag, std::vector<rhi::ShaderDefine> defs, bool shadow,
                        bool forward, bool entity) {
        rhi::GraphicsPipelineDesc d;
        d.name = std::move(name);
        d.vertex = rhi::ShaderStageDesc::file(vert, defs);
        d.fragment = rhi::ShaderStageDesc::file(frag, defs);
        d.raster.cullMode = VK_CULL_MODE_NONE;
        if (shadow) {
            d.depthFormat = formats::kShadowDepth;
            d.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
            d.raster.depthBias = true;
            d.raster.depthClamp = dev.caps().depthClamp;
        } else if (forward) {
            d.colorFormats = {formats::kSceneColor};
            d.depthFormat = formats::kDepth;
            d.depth = {true, false, VK_COMPARE_OP_EQUAL};
        } else {
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
        }
        return dev.createGraphicsPipeline(d);
    };
    for (u32 alpha = 0; alpha < 2; ++alpha) {
        for (u32 entity = 0; entity < 2; ++entity) {
            std::vector<rhi::ShaderDefine> defs;
            if (alpha) defs.push_back({"OX_ALPHA_TEST"});
            if (entity) defs.push_back({"OX_ENTITY_ID"});
            pipePrepass[alpha][entity] = graphics(std::format("world.vegetation.prepass.a{}e{}", alpha, entity),
                                                  "render/world/vegetation.vert", "render/world/vegetation_prepass.frag",
                                                  defs, false, false, entity);
        }
        std::vector<rhi::ShaderDefine> sdefs{{"OX_VEG_SHADOW"}};
        if (alpha) sdefs.push_back({"OX_ALPHA_TEST"});
        pipeShadow[alpha] = graphics(std::format("world.vegetation.shadow.a{}", alpha), "render/world/vegetation.vert",
                                     "render/world/vegetation_prepass.frag", sdefs, true, false, false);
    }
    pipeForward[0] = graphics("world.vegetation.forward", "render/world/vegetation.vert", "render/world/vegetation.frag", {},
                              false, true, false);
    pipeForward[1] = pipeForward[0];
    for (u32 entity = 0; entity < 2; ++entity) {
        std::vector<rhi::ShaderDefine> defs;
        if (entity) defs.push_back({"OX_ENTITY_ID"});
        impPrepass[entity] = graphics(entity ? "world.impostor.prepass.id" : "world.impostor.prepass",
                                      "render/world/impostor.vert", "render/world/impostor_prepass.frag", defs, false, false,
                                      entity);
    }
    impForward = graphics("world.impostor.forward", "render/world/impostor.vert", "render/world/impostor.frag", {}, false,
                          true, false);
    impShadow = graphics("world.impostor.shadow", "render/world/impostor.vert", "render/world/impostor_prepass.frag",
                         {{"OX_VEG_SHADOW"}}, true, false, false);
    {
        rhi::GraphicsPipelineDesc d;
        d.name = "world.impostor.bake";
        d.vertex = rhi::ShaderStageDesc::file("render/world/impostor_bake.vert");
        d.fragment = rhi::ShaderStageDesc::file("render/world/impostor_bake.frag");
        d.colorFormats = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
        d.depthFormat = VK_FORMAT_D32_SFLOAT;
        d.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
        d.raster.cullMode = VK_CULL_MODE_NONE;
        m_bake = dev.createGraphicsPipeline(d);
    }
    return true;
}

void VegetationSystem::shutdown(rhi::Device& dev) {
    for (Proto& p : m_protos) {
        if (p.albedoAtlas) dev.destroy(p.albedoAtlas);
        if (p.normalAtlas) dev.destroy(p.normalAtlas);
    }
    m_protos.clear();
    m_protoIndex.clear();
    m_batches.clear();
    m_arena.release(dev);
    dev.destroy(m_quadIndices);
    for (rhi::PipelineHandle p : {m_cull, m_args, m_bake, pipePrepass[0][0], pipePrepass[0][1], pipePrepass[1][0],
                                  pipePrepass[1][1], pipeForward[0], pipeShadow[0], pipeShadow[1], impPrepass[0],
                                  impPrepass[1], impForward, impShadow}) {
        if (p) dev.destroy(p);
    }
}

void VegetationSystem::resolveMeshes(GpuResourceCache& cache, GpuScene& scene, Proto& p, const VegetationPrototypeDesc* desc) {
    std::vector<Uuid> lods;
    if (desc && !desc->lods.empty()) {
        lods = desc->lods;
    } else {
        if (!m_builtin) m_builtin = &registerBuiltinVegetation(cache); // first prototype without its own meshes
        const Uuid* b = p.kind == world::VegetationKind::Tree    ? m_builtin->treeLods
                        : p.kind == world::VegetationKind::Grass ? m_builtin->grassLods
                                                                 : m_builtin->bushLods;
        lods.assign(b, b + 3);
    }
    u64 key = 0;
    for (u32 l = 0; l < 3; ++l) {
        const Uuid& id = lods[std::min<usize>(l, lods.size() - 1)];
        MeshLod& ml = p.lods[l];
        ml.mesh = cache.mesh(id);
        ml.materials.clear();
        ml.variants.clear();
        key = hashCombine(key, std::hash<Uuid>{}(id));
        if (!ml.mesh) continue;
        for (u32 s = 0; s < ml.mesh->submeshCount; ++s) {
            Uuid mat = desc && desc->material.isValid() ? desc->material
                       : s < ml.mesh->slotMaterials.size() ? ml.mesh->slotMaterials[s]
                                                          : Uuid{};
            const u32 idx = mat.isValid() ? cache.materialIndex(mat) : scene.defaultMaterial();
            ml.materials.push_back(idx);
            const GpuMaterial& gm = scene.material(idx);
            ml.variants.push_back((gm.flags & kMaterialBlendMask) == 1u ? u32(kVariantAlphaTest) : 0u);
        }
    }
    if (p.lods[0].mesh) {
        const AABB& b = p.lods[0].mesh->bounds;
        const f32 hx = std::max(std::abs(b.min.x), std::abs(b.max.x)), hz = std::max(std::abs(b.min.z), std::abs(b.max.z));
        p.height = std::max(b.max.y, 0.05f);
        p.centerY = (b.min.y + b.max.y) * 0.5f;
        p.radius = std::max({hx, hz, (b.max.y - b.min.y) * 0.5f}) * 1.03f;
    }
    if (key != p.meshKey) {
        p.meshKey = key;
        p.baked = false;
    }
}

u32 VegetationSystem::protoSlot(GpuResourceCache& cache, u16 prototype, world::VegetationKind kind, const WorldSnapshot& world) {
    const auto k = std::make_pair(u32(prototype), u32(kind));
    auto it = m_protoIndex.find(k);
    u32 slot;
    if (it == m_protoIndex.end()) {
        slot = u32(m_protos.size());
        m_protos.emplace_back();
        m_protos.back().prototype = prototype;
        m_protos.back().kind = kind;
        m_protoIndex.emplace(k, slot);
    } else {
        slot = it->second;
    }
    Proto& p = m_protos[slot];
    const VegetationPrototypeDesc* desc = nullptr;
    for (const VegetationPrototypeDesc& d : world.prototypes) {
        if (d.prototype == prototype) desc = &d;
    }
    resolveMeshes(cache, m_renderer->scene(), p, desc);
    p.impostor = desc ? desc->impostor && kind != world::VegetationKind::Grass : kind == world::VegetationKind::Tree;
    p.windSway = desc ? desc->windSway : 1.0f;
    p.windFlutter = desc ? desc->windFlutter : 1.0f;
    p.translucency = desc ? desc->translucency : (kind == world::VegetationKind::Grass ? 0.5f : 0.6f);
    p.castShadows = desc ? desc->castShadows : true;
    return slot;
}

void VegetationSystem::update(FeatureContext& ctx, const WorldSnapshot* world, const WorldCVars& cv) {
    OX_PROFILE_ZONE();
    rhi::Device& dev = ctx.device();
    GpuResourceCache& cache = ctx.renderer().resources();
    ++m_frame;
    // Deferred frees: a region may still be read by frames in flight.
    for (auto it = m_pendingFrees.begin(); it != m_pendingFrees.end();) {
        if (it->first + dev.framesInFlight() + 1 <= m_frame) {
            m_arena.free(it->second.first, it->second.second);
            it = m_pendingFrees.erase(it);
        } else {
            ++it;
        }
    }
    m_layers.clear();
    m_frameBatches.clear();
    m_instanceCount = 0;
    m_interactors.clear();
    m_hasWind = false;
    if (world && cv.foliage) {
        const f32 dt = std::max(ctx.snapshot().deltaTime, 0.0f);
        m_time = f32(std::fmod(world->time, 3600.0));
        m_prevTime = m_time - dt;
        m_hasWind = world->hasWind;
        if (m_hasWind) {
            m_frameWind[0] = world->wind.dirSpeedTime;
            m_frameWind[1] = world->wind.gust;
            m_frameWind[0].w = m_time;
            m_prevWind[0] = m_frameWind[0];
            m_prevWind[1] = m_frameWind[1];
            m_prevWind[0].w = m_prevTime;
        }
        m_interactors.assign(world->interactors.begin(), world->interactors.begin() + std::min<usize>(world->interactors.size(), 8));
        for (const VegetationSnapshot& v : world->vegetation) {
            const u32 layerBase = u32(m_layers.size());
            for (const VegetationLayerSnapshot& l : v.layers) {
                LayerSlot s;
                s.proto = protoSlot(cache, l.prototype, l.kind, *world);
                s.kind = l.kind;
                s.castsShadow = l.castsShadow && m_protos[s.proto].castShadows;
                s.lod = l.lod;
                const f32 ds = std::max(cv.foliageDrawDistanceScale, 0.05f);
                s.lod.lodDistances[0] *= ds;
                s.lod.lodDistances[1] *= ds;
                s.lod.cullDistance *= ds;
                s.lod.impostorDistance *= std::max(cv.foliageImpostorDistanceScale, 0.05f);
                const Proto& p = m_protos[s.proto];
                s.impostors = cv.foliageImpostors && p.impostor && p.baked && s.lod.impostorDistance > 0.0f &&
                              s.lod.impostorDistance < s.lod.cullDistance;
                if (s.impostors) s.lod.impostorDistance = std::max(s.lod.impostorDistance, s.lod.lodDistances[1]);
                s.density = l.kind == world::VegetationKind::Tree ? 1.0f : std::clamp(cv.foliageDensity, 0.0f, 1.0f);
                s.enabled = (cv.foliageGrass || l.kind != world::VegetationKind::Grass) && p.lods[0].mesh != nullptr;
                m_layers.push_back(s);
            }
            for (const VegetationBatchSnapshot& b : v.batches) {
                if (!b.instances || b.instances->empty() || !b.cells) continue;
                BatchGpu& g = m_batches[b.key];
                if (g.version != b.version || g.count != b.instances->size()) {
                    if (g.count) m_pendingFrees.push_back({m_frame, {g.offset, g.count}});
                    g.count = u32(b.instances->size());
                    g.offset = m_arena.allocate(dev, g.count);
                    g.version = b.version;
                    dev.uploadBufferAsync(m_arena.buffer(),
                                          std::span<const u8>(reinterpret_cast<const u8*>(b.instances->data()),
                                                              b.instances->size() * sizeof(world::VegetationInstanceGpu)),
                                          g.offset * sizeof(world::VegetationInstanceGpu));
                }
                g.cells = b.cells;
                g.cellLayerSlot.assign(b.cells->size(), ~0u);
                for (usize c = 0; c < b.cells->size(); ++c) {
                    const u16 layer = (*b.cells)[c].layer;
                    if (layer < v.layers.size()) g.cellLayerSlot[c] = layerBase + layer;
                }
                g.lastSeen = m_frame;
                m_frameBatches.push_back(b.key);
                m_instanceCount += g.count;
            }
        }
    }
    for (auto it = m_batches.begin(); it != m_batches.end();) {
        if (it->second.lastSeen != m_frame) {
            if (it->second.count) m_pendingFrees.push_back({m_frame, {it->second.offset, it->second.count}});
            it = m_batches.erase(it);
        } else {
            ++it;
        }
    }
    // Impostors to bake (in-frame graph pass, once per prototype mesh set).
    m_pendingBakes.clear();
    if (cv.foliageImpostors) {
        for (u32 i = 0; i < m_protos.size(); ++i) {
            const Proto& p = m_protos[i];
            if (p.impostor && !p.baked && p.lods[0].mesh) m_pendingBakes.push_back(i);
        }
    }
}

void VegetationSystem::declareBakes(FeatureContext& ctx) {
    if (m_pendingBakes.empty()) return;
    rhi::Device& dev = ctx.device();
    const VkDeviceAddress sceneAddr = ctx.sceneAddress();
    const rhi::BufferHandle indexBuffer = ctx.scene().indexBuffer();
    struct BakeItem {
        u32 proto;
        rhi::TextureHandle albedo, normal, depth;
    };
    auto items = std::make_shared<std::vector<BakeItem>>();
    const u32 atlas = kImpostorFrames * kImpostorFrameSize;
    for (u32 slot : m_pendingBakes) {
        Proto& p = m_protos[slot];
        using U = rhi::TextureUsage;
        if (!p.albedoAtlas) {
            rhi::TextureDesc d;
            d.width = d.height = atlas;
            d.mipLevels = rhi::fullMipCount(atlas, atlas);
            d.usage = U::ColorAttachment | U::Sampled | U::TransferSrc | U::TransferDst;
            d.format = VK_FORMAT_R8G8B8A8_SRGB;
            d.name = "world.impostor.albedo";
            p.albedoAtlas = dev.createTexture(d);
            d.format = VK_FORMAT_R8G8B8A8_UNORM;
            d.name = "world.impostor.normal";
            p.normalAtlas = dev.createTexture(d);
        }
        rhi::TextureDesc dd;
        dd.width = dd.height = atlas;
        dd.format = VK_FORMAT_D32_SFLOAT;
        dd.usage = U::DepthStencilAttachment;
        dd.name = "world.impostor.bakeDepth";
        items->push_back({slot, p.albedoAtlas, p.normalAtlas, dev.createTexture(dd)});
        p.baked = true;
        p.impostorFrames = kImpostorFrames;
    }
    m_pendingBakes.clear();
    ctx.graph().addPass("Vegetation.ImpostorBake").sideEffect().execute([this, items, sceneAddr, indexBuffer, atlas](rhi::PassContext& pc) {
        rhi::CommandList& cmd = pc.cmd;
        for (const BakeItem& it : *items) {
            const Proto& p = m_protos[it.proto];
            const MeshLod& ml = p.lods[0];
            if (!ml.mesh) continue;
            cmd.transition(it.albedo, rhi::Access::ColorAttachmentWrite, true);
            cmd.transition(it.normal, rhi::Access::ColorAttachmentWrite, true);
            cmd.transition(it.depth, rhi::Access::DepthStencilWrite, true);
            rhi::RenderingDesc rd;
            rd.colors = {rhi::ColorAttachment{it.albedo, 0, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {}},
                         rhi::ColorAttachment{it.normal, 0, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                              rhi::ClearColor::rgba(0.5f, 1.0f, 0.5f, 0.0f)}};
            rd.depth = rhi::DepthAttachment{it.depth, 0, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, {0.0f, 0}};
            cmd.beginRendering(rd);
            cmd.bindPipeline(m_bake);
            cmd.bindIndexBuffer(indexBuffer);
            const glm::vec3 center(0.0f, p.centerY, 0.0f);
            const f32 r = p.radius;
            for (u32 j = 0; j < kImpostorFrames; ++j) {
                for (u32 i = 0; i < kImpostorFrames; ++i) {
                    const glm::vec2 oct = (glm::vec2(f32(i), f32(j)) + 0.5f) / f32(kImpostorFrames) * 2.0f - 1.0f;
                    const glm::vec3 d = hemiOctDecode(oct);
                    glm::vec3 right, up;
                    impostorBasis(d, right, up);
                    // Orthographic, reversed-Z (towards the viewer = 1), Vulkan y down: up → top of the frame.
                    glm::mat4 m(0.0f);
                    m[0] = glm::vec4(right.x / r, -up.x / r, 0.5f * d.x / r, 0.0f);
                    m[1] = glm::vec4(right.y / r, -up.y / r, 0.5f * d.y / r, 0.0f);
                    m[2] = glm::vec4(right.z / r, -up.z / r, 0.5f * d.z / r, 0.0f);
                    m[3] = glm::vec4(-glm::dot(center, right) / r, glm::dot(center, up) / r,
                                     0.5f - 0.5f * glm::dot(center, d) / r, 1.0f);
                    cmd.setViewport(f32(i * kImpostorFrameSize), f32(j * kImpostorFrameSize), f32(kImpostorFrameSize),
                                    f32(kImpostorFrameSize));
                    cmd.setScissor(i32(i * kImpostorFrameSize), i32(j * kImpostorFrameSize), kImpostorFrameSize,
                                   kImpostorFrameSize);
                    for (u32 s = 0; s < ml.mesh->submeshCount; ++s) {
                        const GpuMeshInfo& mi = m_renderer->scene().meshInfo(ml.mesh->firstMeshInfo + s);
                        struct {
                            u64 scene;
                            glm::mat4 viewProj;
                            u32 material, pad;
                        } push{sceneAddr, m, s < ml.materials.size() ? ml.materials[s] : 0u, 0};
                        cmd.pushConstants(push);
                        cmd.drawIndexed(mi.indexCount, 1, mi.firstIndex, mi.vertexOffset, 0);
                    }
                }
            }
            cmd.endRendering();
            cmd.generateMipmaps(it.albedo, rhi::Access::SampledGraphics);
            cmd.generateMipmaps(it.normal, rhi::Access::SampledGraphics);
            pc.device.destroy(it.depth);
        }
        (void)atlas;
    });
}

std::shared_ptr<VegetationSystem::ViewData> VegetationSystem::prepare(FeatureContext& ctx, const glm::vec3& lodCamera,
                                                                      std::span<const glm::mat4> cullViewProjs,
                                                                      bool shadow, const WorldCVars& cv) {
    if (m_frameBatches.empty() || m_layers.empty()) return nullptr;
    if (shadow && !cv.foliageShadows) return nullptr;
    OX_PROFILE_ZONE();
    rhi::Device& dev = ctx.device();
    auto out = std::make_shared<ViewData>();
    const WorldSnapshot* world = ctx.snapshot().findExtension<WorldSnapshot>();
    out->entityId = world && !world->vegetation.empty() ? world->vegetation[0].entityId : 0;

    // Per-frame constants shared by every job.
    VegFrameGpu f{};
    f.wind[0] = m_frameWind[0];
    f.wind[1] = m_frameWind[1];
    f.windPrev[0] = m_prevWind[0];
    f.windPrev[1] = m_prevWind[1];
    for (usize i = 0; i < m_interactors.size(); ++i) f.interactors[i] = m_interactors[i];
    f.interactorCount = u32(m_interactors.size());
    f.time = m_time;
    f.prevTime = m_prevTime;
    f.hasWind = m_hasWind ? 1u : 0u;
    out->frame = ctx.upload(std::span<const VegFrameGpu>(&f, 1));
    std::vector<VegProtoGpu> protos(m_protos.size());
    for (usize i = 0; i < m_protos.size(); ++i) {
        const Proto& p = m_protos[i];
        protos[i].wind = {p.windSway, p.windFlutter, p.translucency, p.height};
        protos[i].impostor = {f32(p.impostorFrames), p.radius, p.centerY, 0.5f};
        protos[i].albedoAtlas = p.albedoAtlas ? dev.sampledIndex(p.albedoAtlas) : kInvalidIndex;
        protos[i].normalAtlas = p.normalAtlas ? dev.sampledIndex(p.normalAtlas) : kInvalidIndex;
        protos[i].kind = u32(p.kind);
    }
    out->protos = ctx.upload(std::span<const VegProtoGpu>(protos));

    // Layers (same for every job of the frame).
    std::vector<CullLayerGpu> layers(m_layers.size());
    for (usize i = 0; i < m_layers.size(); ++i) {
        const LayerSlot& s = m_layers[i];
        layers[i].lodEnds = {s.lod.lodDistances[0], s.lod.lodDistances[1], s.impostors ? s.lod.impostorDistance : s.lod.cullDistance,
                             s.lod.cullDistance};
        layers[i].params = {s.lod.fadeRange, s.density, s.impostors ? 1.0f : 0.0f, 0.0f};
        layers[i].protoSlot = s.proto;
    }
    const VkDeviceAddress layersAddr = ctx.upload(std::span<const CullLayerGpu>(layers));
    const VkDeviceAddress instancesAddr = m_arena.address();
    const u32 slotCount = u32(m_protos.size()) * kLods;

    for (const glm::mat4& vp : cullViewProjs) {
        CullJob job;
        const world::Frustum frustum = world::Frustum::fromViewProjection(vp);
        std::vector<CullCellGpu> cells;
        std::vector<u64> capacity(slotCount, 0);
        for (u64 key : m_frameBatches) {
            const BatchGpu& b = m_batches.at(key);
            for (usize c = 0; c < b.cells->size(); ++c) {
                const u32 ls = b.cellLayerSlot[c];
                if (ls == ~0u) continue;
                const LayerSlot& L = m_layers[ls];
                if (!L.enabled || (shadow && !L.castsShadow)) continue;
                const world::VegetationCell& cell = (*b.cells)[c];
                if (cell.count == 0) continue;
                // Instances may stick out of the cell bounds by their radius: the GPU test is exact.
                world::Aabb box = cell.bounds;
                const f32 dmin = std::sqrt(world::distanceSq(box, lodCamera));
                if (dmin > L.lod.cullDistance) continue;
                if (!frustum.intersects(box)) continue;
                const glm::vec3 farCorner = glm::max(glm::abs(box.min - lodCamera), glm::abs(box.max - lodCamera));
                const f32 dmax = glm::length(farCorner);
                const u32 l0 = lodAt(L.lod, L.impostors, dmin);
                u32 l1 = lodAt(L.lod, L.impostors, dmax);
                if (l1 >= 4) l1 = L.impostors ? 3u : 2u;
                for (u32 l = l0; l <= std::min(l1 + 1, 3u); ++l) capacity[L.proto * kLods + l] += cell.count;
                cells.push_back({u32(b.offset) + cell.first, cell.count, ls, 0});
            }
        }
        if (cells.empty()) {
            out->jobs.push_back(std::move(job));
            continue;
        }
        // Slots + commands.
        std::vector<SlotGpu> slots(slotCount);
        u64 total = 0;
        for (u32 s = 0; s < slotCount; ++s) {
            slots[s] = {u32(total), u32(capacity[s]), 0, 0};
            total += capacity[s];
        }
        std::vector<VkDrawIndexedIndirectCommand> args;
        std::vector<u32> commandSlots;
        for (u32 pi = 0; pi < m_protos.size(); ++pi) {
            const Proto& p = m_protos[pi];
            for (u32 l = 0; l < kLods; ++l) {
                const u32 s = pi * kLods + l;
                if (capacity[s] == 0) continue;
                if (l == 3 && p.baked) {
                    Command c{pi, 3, 0, 0, 2};
                    job.commands.push_back(c);
                    args.push_back({6, 0, 0, 0, slots[s].base});
                    commandSlots.push_back(s);
                    continue;
                }
                const MeshLod& ml = p.lods[std::min(l, 2u)];
                if (!ml.mesh) continue;
                for (u32 sm = 0; sm < ml.mesh->submeshCount; ++sm) {
                    const GpuMeshInfo& mi = m_renderer->scene().meshInfo(ml.mesh->firstMeshInfo + sm);
                    Command c{pi, std::min(l, 2u), sm < ml.materials.size() ? ml.materials[sm] : 0u,
                              sm < ml.variants.size() ? ml.variants[sm] : 0u, mi.indexCount / 3};
                    job.commands.push_back(c);
                    args.push_back({mi.indexCount, 0, mi.firstIndex, mi.vertexOffset, slots[s].base});
                    commandSlots.push_back(s);
                }
            }
        }
        if (job.commands.empty()) {
            out->jobs.push_back(std::move(job));
            continue;
        }
        // Indirect buffer layout: [slots][args (20 B each)][command → slot].
        const u64 slotsBytes = u64(slotCount) * sizeof(SlotGpu);
        const u64 argsBytes = args.size() * sizeof(VkDrawIndexedIndirectCommand);
        const u64 initBytes = slotsBytes + argsBytes + commandSlots.size() * 4;
        GpuAllocation init = ctx.allocate(initBytes, 16);
        std::memcpy(init.cpu, slots.data(), slotsBytes);
        std::memcpy(static_cast<u8*>(init.cpu) + slotsBytes, args.data(), argsBytes);
        std::memcpy(static_cast<u8*>(init.cpu) + slotsBytes + argsBytes, commandSlots.data(), commandSlots.size() * 4);
        job.argsOffset = slotsBytes;
        using BU = rhi::BufferUsage;
        job.records = ctx.graph().createBuffer({bucket(total, 4096) * 8, BU::Storage, rhi::MemoryUsage::GpuOnly, "Vegetation.Records"});
        job.indirect = ctx.graph().createBuffer({bucket(initBytes, 4096), BU::Storage | BU::Indirect | BU::TransferDst,
                                                 rhi::MemoryUsage::GpuOnly, "Vegetation.Indirect"});
        const VkDeviceAddress cellsAddr = ctx.upload(std::span<const CullCellGpu>(cells));
        const VkDeviceAddress planesAddr = ctx.upload(std::span<const glm::vec4>(frustum.planes.data(), 6));
        const u32 cellCount = u32(cells.size());
        const u32 commandCount = u32(job.commands.size());
        const rhi::RGBuffer records = job.records, indirect = job.indirect;
        const glm::vec3 cam = lodCamera;
        ctx.graph()
            .addPass(shadow ? "Vegetation.CullShadow" : "Vegetation.Cull", rhi::PassType::Compute)
            .overwrite(records, rhi::Access::StorageWriteCompute)
            .overwrite(indirect, rhi::Access::StorageWriteCompute)
            .execute([this, init, initBytes, records, indirect, cellsAddr, layersAddr, instancesAddr, planesAddr, cam, cellCount,
                      slotsBytes, argsBytes, commandCount](rhi::PassContext& p) {
                const rhi::BufferHandle ib = p.buffer(indirect);
                const VkDeviceAddress ia = p.address(indirect);
                p.cmd.copyBuffer(init.buffer, ib, initBytes, init.offset, 0);
                p.cmd.memoryBarrier(rhi::Access::TransferWrite, rhi::Access::StorageWriteCompute);
                p.cmd.bindPipeline(m_cull);
                p.cmd.pushConstants(CullPush{cellsAddr, layersAddr, instancesAddr, planesAddr, ia, p.address(records), cam, cellCount});
                p.cmd.dispatch(cellCount);
                p.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
                p.cmd.bindPipeline(m_args);
                p.cmd.pushConstants(ArgsPush{ia, ia + slotsBytes, ia + slotsBytes + argsBytes, commandCount, 0});
                p.cmd.dispatch((commandCount + 63) / 64);
            });
        out->jobs.push_back(std::move(job));
    }
    bool any = false;
    for (const CullJob& j : out->jobs) any |= !j.empty();
    return any ? out : nullptr;
}

void VegetationSystem::declareReads(rhi::PassBuilder& pass, const ViewData& data) const {
    for (const CullJob& j : data.jobs) {
        if (j.empty()) continue;
        pass.read(j.records, rhi::Access::StorageReadGraphics);
        pass.read(j.indirect, rhi::Access::IndirectBuffer);
    }
}

void VegetationSystem::draw(rhi::PassContext& p, const ViewData& data, u32 jobIndex, WorldPass pass, const WorldPassContext& wc,
                            FeatureContext& fc) const {
    if (jobIndex >= data.jobs.size()) return;
    const CullJob& job = data.jobs[jobIndex];
    if (job.empty()) return;
    rhi::CommandList& cmd = p.cmd;
    const rhi::BufferHandle indirect = p.buffer(job.indirect);
    VegPush pc;
    pc.view = wc.view;
    pc.scene = wc.scene;
    pc.instances = m_arena.address();
    pc.records = p.address(job.records);
    pc.frame = data.frame;
    pc.protos = data.protos;
    pc.matrices = wc.matrix;
    pc.entityId = data.entityId;
    pc.lightDirOct = wc.lightDirOct;
    for (u32 i = 0; i < 4; ++i) pc.inputs[i] = wc.inputs[i];
    rhi::PipelineHandle bound;
    bool quadBound = false, sceneBound = false;
    for (u32 c = 0; c < job.commands.size(); ++c) {
        const Command& cm = job.commands[c];
        const bool impostor = cm.lod == 3;
        const u32 alpha = (cm.variant & kVariantAlphaTest) ? 1u : 0u;
        rhi::PipelineHandle pipe;
        switch (pass) {
        case WorldPass::Prepass: pipe = impostor ? impPrepass[wc.entityIds ? 1 : 0] : pipePrepass[alpha][wc.entityIds ? 1 : 0]; break;
        case WorldPass::Forward: pipe = impostor ? impForward : pipeForward[0]; break;
        case WorldPass::Shadow: pipe = impostor ? impShadow : pipeShadow[alpha]; break;
        }
        if (!(pipe == bound)) {
            cmd.bindPipeline(pipe);
            bound = pipe;
        }
        if (impostor && !quadBound) {
            cmd.bindIndexBuffer(m_quadIndices);
            quadBound = true;
            sceneBound = false;
        } else if (!impostor && !sceneBound) {
            cmd.bindIndexBuffer(wc.sceneIndexBuffer);
            sceneBound = true;
            quadBound = false;
        }
        pc.proto = cm.proto;
        pc.material = cm.material;
        cmd.pushConstants(pc);
        cmd.drawIndexedIndirect(indirect, job.argsOffset + u64(c) * sizeof(VkDrawIndexedIndirectCommand), 1,
                                sizeof(VkDrawIndexedIndirectCommand));
        fc.countDraw(0, 0);
    }
}

} // namespace ox::render::worldfx

#endif // OX_RENDER_HAS_WORLD
