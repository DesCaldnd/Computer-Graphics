#include <oxwald/core/jobs.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/render/gpu_resource_cache.hpp>
#include <oxwald/render/mesh_primitives.hpp>
#if OX_RENDER_HAS_ASSETS
#include <oxwald/assets/texture_import.hpp>
#endif
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/format.hpp>

#include <cstring>
#include <thread>

namespace ox::render {

namespace {

VkFormat toVk(assets::TextureFormat f) {
    using F = assets::TextureFormat;
    switch (f) {
    case F::R8Unorm: return VK_FORMAT_R8_UNORM;
    case F::RG8Unorm: return VK_FORMAT_R8G8_UNORM;
    case F::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case F::RGBA8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case F::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case F::BC5Unorm: return VK_FORMAT_BC5_UNORM_BLOCK;
    case F::BC7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    case F::BC7Srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
    case F::BC6HUfloat: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case F::R16Unorm: return VK_FORMAT_R16_UNORM;
    case F::R32Float: return VK_FORMAT_R32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

bool isBlockCompressed(VkFormat f) { return rhi::formatInfo(f).compressed; }

rhi::TextureHandle makeSolid(rhi::Device& dev, const char* name, VkFormat format, const void* texel, u32 bytes, u32 size,
                             bool cube = false) {
    rhi::TextureDesc d;
    d.name = name;
    d.format = format;
    d.width = d.height = size;
    d.type = cube ? rhi::TextureType::Cube : rhi::TextureType::Tex2D;
    d.arrayLayers = cube ? 6 : 1;
    d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    rhi::TextureHandle t = dev.createTexture(d);
    std::vector<u8> data(usize(size) * size * bytes * d.arrayLayers);
    for (usize i = 0; i < data.size(); i += bytes) std::memcpy(&data[i], texel, bytes);
    dev.uploadTexture(t, data);
    return t;
}

} // namespace

GpuResourceCache::GpuResourceCache(rhi::Device& device, GpuScene& scene) : m_device(&device), m_scene(&scene) {
    createDefaults();
    setTextureQuality(8, 0.0f, 8192);
    // Built-in primitives are always resident.
    for (u32 p = 0; p < u32(Primitive::Count); ++p) addMesh(primitiveUuid(Primitive(p)), makePrimitive(Primitive(p)));
}

GpuResourceCache::~GpuResourceCache() {
    // Wait for in-flight loads that would push into m_completed.
    while (m_inFlight.load() > 0) std::this_thread::yield();
    for (auto& [id, e] : m_textures) {
        if (!e.external && e.gpu.texture) m_device->destroy(e.gpu.texture);
    }
    for (rhi::TextureHandle t : {m_defaults.white, m_defaults.black, m_defaults.flatNormal, m_defaults.checker,
                                 m_defaults.blackCube}) {
        if (t) m_device->destroy(t);
    }
}

void GpuResourceCache::createDefaults() {
    rhi::Device& dev = *m_device;
    const u8 white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255}, flat[4] = {128, 128, 255, 255};
    m_defaults.white = makeSolid(dev, "default.white", VK_FORMAT_R8G8B8A8_SRGB, white, 4, 4);
    m_defaults.black = makeSolid(dev, "default.black", VK_FORMAT_R8G8B8A8_SRGB, black, 4, 4);
    m_defaults.flatNormal = makeSolid(dev, "default.flatNormal", VK_FORMAT_R8G8B8A8_UNORM, flat, 4, 4);
    const u16 zeroHalf[4] = {0, 0, 0, 0x3C00};
    m_defaults.blackCube = makeSolid(dev, "default.blackCube", VK_FORMAT_R16G16B16A16_SFLOAT, zeroHalf, 8, 1, true);
    {
        rhi::TextureDesc d;
        d.name = "default.checker";
        d.format = VK_FORMAT_R8G8B8A8_SRGB;
        d.width = d.height = 64;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        m_defaults.checker = dev.createTexture(d);
        std::vector<u8> px(64 * 64 * 4);
        for (u32 y = 0; y < 64; ++y)
            for (u32 x = 0; x < 64; ++x) {
                const bool on = ((x / 8) + (y / 8)) & 1;
                u8* p = &px[(y * 64 + x) * 4];
                p[0] = on ? 255 : 90, p[1] = on ? 0 : 90, p[2] = on ? 255 : 90, p[3] = 255;
            }
        dev.uploadTexture(m_defaults.checker, px);
    }
    m_defaults.whiteIndex = dev.sampledIndex(m_defaults.white);
    m_defaults.blackIndex = dev.sampledIndex(m_defaults.black);
    m_defaults.flatNormalIndex = dev.sampledIndex(m_defaults.flatNormal);
    m_defaults.checkerIndex = dev.sampledIndex(m_defaults.checker);
    m_defaults.blackCubeIndex = dev.sampledIndex(m_defaults.blackCube);
}

void GpuResourceCache::setProvider(AssetProvider provider, JobSystem* jobs) {
    m_provider = std::move(provider);
    m_jobs = jobs;
}

void GpuResourceCache::setTextureQuality(i32 anisotropy, f32 mipBias, i32 maxTextureSize) {
    m_maxTextureSize = maxTextureSize;
    if (anisotropy == m_anisotropy && mipBias == m_mipBias && m_samplerRepeat != 4) return;
    m_anisotropy = anisotropy;
    m_mipBias = mipBias;
    rhi::SamplerDesc sd;
    sd.maxAnisotropy = m_device->caps().samplerAnisotropy ? f32(std::max(anisotropy, 1)) : 0.0f;
    sd.mipLodBias = mipBias;
    m_samplerRepeat = m_device->samplerIndex(m_device->sampler(sd));
    sd.addressU = sd.addressV = sd.addressW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    m_samplerClamp = m_device->samplerIndex(m_device->sampler(sd));
    for (auto& [id, e] : m_materialsById) refreshMaterial(e);
}

// --- registration ---

void GpuResourceCache::addMesh(const Uuid& id, const assets::MeshData& mesh) { applyMesh(id, mesh); }
void GpuResourceCache::addTexture(const Uuid& id, const assets::TextureData& texture) {
    if (m_streaming) {
        auto shared = std::make_shared<const assets::TextureData>(texture); // the streamer keeps the CPU mips
        applyTexture(id, *shared, shared);
    } else {
        applyTexture(id, texture);
    }
}
void GpuResourceCache::addMaterial(const Uuid& id, const assets::MaterialAsset& material) { applyMaterial(id, material); }

void GpuResourceCache::addExternalTexture(const Uuid& id, rhi::TextureHandle texture) {
    TextureEntry& e = m_textures[id];
    if (!e.external && e.gpu.texture) m_device->destroy(e.gpu.texture);
    const rhi::TextureDesc& d = m_device->desc(texture);
    e.external = true;
    e.state = ResourceState::Ready;
    e.gpu = {texture, m_device->sampledIndex(texture), d.type == rhi::TextureType::Cube, d.width, d.height, d.mipLevels};
    for (auto& [mid, m] : m_materialsById) refreshMaterial(m);
}

void GpuResourceCache::remove(const Uuid& id) {
    if (auto it = m_meshes.find(id); it != m_meshes.end()) {
        m_scene->freeMesh(it->second.gpu);
        m_meshes.erase(it);
    }
    if (auto it = m_textures.find(id); it != m_textures.end()) {
        if (!it->second.external && it->second.gpu.texture) m_device->destroy(it->second.gpu.texture);
        m_textures.erase(it);
        if (m_streaming) m_streaming->onTextureRemoved(id);
        for (auto& [mid, m] : m_materialsById) refreshMaterial(m);
    }
    if (auto it = m_materialsById.find(id); it != m_materialsById.end()) {
        m_scene->freeMaterial(it->second.index);
        m_materialsById.erase(it);
    }
}

void GpuResourceCache::invalidate(const Uuid& id) {
    if (auto it = m_meshes.find(id); it != m_meshes.end()) requestLoad(id, 0);
    if (auto it = m_textures.find(id); it != m_textures.end() && !it->second.external) requestLoad(id, 1);
    if (auto it = m_materialsById.find(id); it != m_materialsById.end()) requestLoad(id, 2);
}

// --- lookups ---

const GpuMesh* GpuResourceCache::mesh(const Uuid& id) {
    auto it = m_meshes.find(id);
    if (it == m_meshes.end()) {
        m_meshes[id].state = ResourceState::Unknown;
        requestLoad(id, 0);
        return nullptr;
    }
    return it->second.gpu.submeshCount > 0 ? &it->second.gpu : nullptr;
}

u32 GpuResourceCache::materialIndex(const Uuid& id) {
    auto it = m_materialsById.find(id);
    if (it == m_materialsById.end()) {
        MaterialEntry& e = m_materialsById[id];
        e.index = m_scene->allocateMaterial();
        m_scene->setMaterial(e.index, GpuMaterial{}); // default look while loading
        requestLoad(id, 2);
        return e.index;
    }
    return it->second.index;
}

const GpuTexture* GpuResourceCache::texture(const Uuid& id) {
    auto it = m_textures.find(id);
    if (it == m_textures.end()) {
        m_textures[id].state = ResourceState::Unknown;
        requestLoad(id, 1);
        return nullptr;
    }
    return it->second.state == ResourceState::Ready ? &it->second.gpu : nullptr;
}

ResourceState GpuResourceCache::state(const Uuid& id) const {
    if (auto it = m_meshes.find(id); it != m_meshes.end()) return it->second.state;
    if (auto it = m_textures.find(id); it != m_textures.end()) return it->second.state;
    if (auto it = m_materialsById.find(id); it != m_materialsById.end()) return it->second.state;
    return ResourceState::Unknown;
}

u32 GpuResourceCache::pendingLoads() const { return m_inFlight.load(); }

// --- loading ---

void GpuResourceCache::requestLoad(const Uuid& id, u8 kind) {
    auto setState = [&](ResourceState s) {
        if (kind == 0) m_meshes[id].state = s;
        else if (kind == 1) m_textures[id].state = s;
        else m_materialsById[id].state = s;
    };
    const bool available = (kind == 0 && m_provider.loadMesh) || (kind == 1 && m_provider.loadTexture) ||
                           (kind == 2 && m_provider.loadMaterial);
    if (!available) {
        // Keep entries that were registered directly; otherwise the asset is unknown to this renderer.
        const ResourceState cur = state(id);
        if (cur != ResourceState::Ready) setState(ResourceState::Missing);
        return;
    }
    if (state(id) != ResourceState::Ready) setState(ResourceState::Loading);
    ++m_inFlight;
    auto job = [this, id, kind, provider = m_provider] {
        Completed c;
        c.id = id;
        c.kind = kind;
        if (kind == 0) c.mesh = provider.loadMesh(id);
        else if (kind == 1) c.texture = provider.loadTexture(id);
        else c.material = provider.loadMaterial(id);
        std::lock_guard lock(m_mutex);
        m_completed.push_back(std::move(c));
        --m_inFlight;
    };
    if (m_jobs) m_jobs->submit(std::move(job));
    else job();
}

void GpuResourceCache::queueInvalidate(const Uuid& id) {
    std::lock_guard lock(m_mutex);
    m_invalidations.push_back(id);
}

void GpuResourceCache::update() {
    OX_PROFILE_ZONE();
    std::vector<Completed> done;
    std::vector<Uuid> invalidations;
    {
        std::lock_guard lock(m_mutex);
        done.swap(m_completed);
        invalidations.swap(m_invalidations);
    }
    for (const Uuid& id : invalidations) invalidate(id);
    for (Completed& c : done) {
        if (c.kind == 0) {
            if (c.mesh) applyMesh(c.id, *c.mesh);
            else m_meshes[c.id].state = ResourceState::Missing;
        } else if (c.kind == 1) {
            if (c.texture) applyTexture(c.id, *c.texture, c.texture);
            else {
                m_textures[c.id].state = ResourceState::Missing;
                for (auto& [mid, m] : m_materialsById) refreshMaterial(m);
            }
        } else {
            if (c.material) applyMaterial(c.id, *c.material);
            else m_materialsById[c.id].state = ResourceState::Missing;
        }
    }
}

void GpuResourceCache::flush() {
    while (m_inFlight.load() > 0) std::this_thread::yield();
    update();
    // Materials loaded above may have requested textures.
    while (m_inFlight.load() > 0) std::this_thread::yield();
    update();
}

void GpuResourceCache::applyMesh(const Uuid& id, const assets::MeshData& mesh) {
    MeshEntry& e = m_meshes[id];
    GpuMesh fresh;
    if (!m_scene->uploadMesh(mesh, fresh)) {
        OX_LOG_WARN("render", "mesh {} has no drawable data", id.toString());
        e.state = e.gpu.submeshCount ? ResourceState::Ready : ResourceState::Missing;
        return;
    }
    // The old ranges may still be read by frames in flight; they are only reused after those retire because the
    // range allocator hands out the most recently freed ranges last... keep it simple: free after a full wait.
    if (e.gpu.submeshCount) {
        m_device->waitIdle();
        m_scene->freeMesh(e.gpu);
    }
    e.gpu = std::move(fresh);
    e.state = ResourceState::Ready;
}

void GpuResourceCache::applyTexture(const Uuid& id, const assets::TextureData& texIn,
                                    std::shared_ptr<const assets::TextureData> shared) {
    TextureEntry& e = m_textures[id];
#if OX_RENDER_HAS_ASSETS
    // Devices without BC support: decode on the CPU (assets::decompressToRGBA8).
    assets::TextureData decoded;
    const bool decode = isBlockCompressed(toVk(texIn.format)) && !m_device->caps().textureCompressionBC &&
                        texIn.format != assets::TextureFormat::BC6HUfloat;
    if (decode) decoded = assets::decompressToRGBA8(texIn);
    const assets::TextureData& tex = decode ? decoded : texIn;
    if (decode && shared) shared = std::make_shared<const assets::TextureData>(decoded);
#else
    const assets::TextureData& tex = texIn;
#endif
    const VkFormat format = toVk(tex.format);
    if (format == VK_FORMAT_UNDEFINED || tex.mips.empty() || tex.width == 0) {
        OX_LOG_WARN("render", "texture {}: unsupported or empty data", id.toString());
        e.state = ResourceState::Missing;
        return;
    }
    const rhi::DeviceCaps& caps = m_device->caps();
    if (isBlockCompressed(format) && !caps.textureCompressionBC) {
        OX_LOG_WARN("render", "texture {}: BC formats unsupported on this device", id.toString());
        e.state = ResourceState::Missing;
        return;
    }
    // Skip mips above the texture size limit (r.Textures.MaxSize).
    usize first = 0;
    while (first + 1 < tex.mips.size() &&
           std::max(tex.mips[first].width, tex.mips[first].height) > u32(std::max(m_maxTextureSize, 1))) {
        ++first;
    }
    if (m_streaming && shared && !tex.cube) {
        first = std::clamp<usize>(m_streaming->onTextureLoaded(id, shared, u32(first)), first, tex.mips.size() - 1);
    }
    const assets::TextureMip& top = tex.mips[first];
    rhi::TextureDesc d;
    d.name = tex.cube ? "asset.cube" : "asset.texture";
    d.format = format;
    d.width = top.width;
    d.height = top.height;
    d.type = tex.cube ? rhi::TextureType::Cube : rhi::TextureType::Tex2D;
    d.arrayLayers = tex.cube ? 6 * std::max(tex.layers / 6, 1u) : std::max(tex.layers, 1u);
    d.mipLevels = u32(tex.mips.size() - first);
    d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    std::vector<u8> data;
    for (usize m = first; m < tex.mips.size(); ++m) {
        const auto& md = tex.mips[m].data;
        data.insert(data.end(), reinterpret_cast<const u8*>(md.data()), reinterpret_cast<const u8*>(md.data()) + md.size());
    }
    rhi::TextureHandle t = m_device->createTexture(d);
    m_device->uploadTextureAsync(t, data, {0, ~0u, 0, ~0u, rhi::Access::SampledGraphics});
    if (!e.external && e.gpu.texture) m_device->destroy(e.gpu.texture); // deferred destruction
    e.external = false;
    e.gpu = {t, m_device->sampledIndex(t), tex.cube, d.width, d.height, d.mipLevels};
    e.state = ResourceState::Ready;
    for (auto& [mid, m] : m_materialsById) refreshMaterial(m);
}

void GpuResourceCache::applyMaterial(const Uuid& id, const assets::MaterialAsset& material) {
    MaterialEntry& e = m_materialsById[id];
    if (e.state == ResourceState::Unknown && !e.hasSource && e.index == 0) e.index = m_scene->allocateMaterial();
    e.source = material;
    e.hasSource = true;
    e.state = ResourceState::Ready;
    refreshMaterial(e);
}

u32 GpuResourceCache::resolveTexture(const Uuid& id, u32 fallback) {
    if (!id.isValid()) return kInvalidIndex;
    auto it = m_textures.find(id);
    if (it == m_textures.end()) {
        m_textures[id].state = ResourceState::Unknown;
        requestLoad(id, 1);
        it = m_textures.find(id);
    }
    if (it->second.state == ResourceState::Ready) return it->second.gpu.sampledIndex;
    if (it->second.state == ResourceState::Missing) return fallback;
    return kInvalidIndex; // loading: material constants only
}

void GpuResourceCache::refreshMaterial(MaterialEntry& e) {
    if (!e.hasSource) return;
    const assets::MaterialAsset& m = e.source;
    GpuMaterial g;
    g.baseColor = m.baseColor;
    g.emissive = m.emissive * m.emissiveStrength;
    g.metallic = m.metallic;
    g.roughness = m.roughness;
    g.normalStrength = m.normalStrength;
    g.occlusionStrength = m.occlusionStrength;
    g.alphaCutoff = m.alphaCutoff;
    g.albedoTexture = resolveTexture(m.albedoTexture, m_defaults.checkerIndex);
    g.normalTexture = resolveTexture(m.normalTexture, kInvalidIndex);
    g.ormTexture = resolveTexture(m.ormTexture, kInvalidIndex);
    g.emissiveTexture = resolveTexture(m.emissiveTexture, kInvalidIndex);
    g.flags = u32(m.blendMode) & kMaterialBlendMask;
    if (m.doubleSided) g.flags |= kMaterialDoubleSided;
    if (m.shadingModel == assets::ShadingModel::Unlit) g.flags |= kMaterialUnlit;
    if (m.renderQueueOffset != 0) g.flags |= kMaterialSorted;
    g.sampler = m_samplerRepeat;
    g.ior = m.ior;
    g.transmission = m.transmission;
    g.absorptionColor = m.absorptionColor;
    g.absorptionDistance = m.absorptionDistance;
    g.thickness = m.thickness;
    g.clearcoat = m.clearcoat;
    g.clearcoatRoughness = m.clearcoatRoughness;
    g.subsurface = m.subsurface;
    g.uvTiling = m.uvTiling;
    g.uvOffset = m.uvOffset;
    m_scene->setMaterial(e.index, g);
}

GpuTexture GpuResourceCache::createTextureFromData(const assets::TextureData& tex, u32 firstMip) {
    const VkFormat format = toVk(tex.format);
    if (format == VK_FORMAT_UNDEFINED || tex.mips.empty() || firstMip >= tex.mips.size()) return {};
    if (isBlockCompressed(format) && !m_device->caps().textureCompressionBC) return {};
    const assets::TextureMip& top = tex.mips[firstMip];
    rhi::TextureDesc d;
    d.name = "asset.texture.streamed";
    d.format = format;
    d.width = top.width;
    d.height = top.height;
    d.type = tex.cube ? rhi::TextureType::Cube : rhi::TextureType::Tex2D;
    d.arrayLayers = tex.cube ? 6 * std::max(tex.layers / 6, 1u) : std::max(tex.layers, 1u);
    d.mipLevels = u32(tex.mips.size() - firstMip);
    d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    std::vector<u8> data;
    for (usize m = firstMip; m < tex.mips.size(); ++m) {
        const auto& md = tex.mips[m].data;
        data.insert(data.end(), reinterpret_cast<const u8*>(md.data()), reinterpret_cast<const u8*>(md.data()) + md.size());
    }
    rhi::TextureHandle t = m_device->createTexture(d);
    m_device->uploadTextureAsync(t, data, {0, ~0u, 0, ~0u, rhi::Access::SampledGraphics});
    return {t, m_device->sampledIndex(t), tex.cube, d.width, d.height, d.mipLevels};
}

bool GpuResourceCache::replaceTexture(const Uuid& id, const GpuTexture& texture) {
    auto it = m_textures.find(id);
    if (it == m_textures.end() || it->second.external || it->second.state != ResourceState::Ready) return false;
    if (it->second.gpu.texture) m_device->destroy(it->second.gpu.texture); // deferred until frames in flight retire
    it->second.gpu = texture;
    for (auto& [mid, m] : m_materialsById) refreshMaterial(m);
    return true;
}

const GpuTexture* GpuResourceCache::residentTexture(const Uuid& id) const {
    auto it = m_textures.find(id);
    return it != m_textures.end() && it->second.state == ResourceState::Ready ? &it->second.gpu : nullptr;
}

u64 GpuResourceCache::textureBytes() const {
    u64 bytes = 0;
    for (const auto& [id, e] : m_textures) {
        if (e.external || !e.gpu.texture) continue;
        const rhi::TextureDesc& d = m_device->desc(e.gpu.texture);
        bytes += rhi::estimateTextureSize(d);
    }
    return bytes;
}

} // namespace ox::render
