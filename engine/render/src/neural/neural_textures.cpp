// Neural texture compression at run time (r.NeuralTextures): resolves materials with a NeuralTextureSet through the
// GpuResourceCache material hook. Off / DecodeOnLoad decode the set into BC textures on worker threads,
// InferenceOnSample keeps latents (2D arrays, B4G4R4A4 or RGBA8) + FP16 weights resident and publishes the
// GpuNeuralMaterial table (VIEW.neuralMaterials) read by render/neural/ntc.glsl. The BC fallback tail of every set
// is registered as regular cache textures (streamed like any other texture) and used while decoding / loading and by
// every consumer without inference.
#include "neural_internal.hpp"

#include <oxwald/assets/neural/ntc.hpp>
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/jobs.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/time.hpp>
#include <oxwald/render/gpu_resource_cache.hpp>
#include <oxwald/render/gpu_scene.hpp>
#include <oxwald/render/neural/neural.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/render/renderer.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/format.hpp>
#include <oxwald/rhi/vulkan.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace ox::render::neural {

namespace {

using S = Scalability;

CVar<int> cvMode("r.NeuralTextures", 1,
                 "Neural texture compression: Off (regular textures), DecodeOnLoad (decode to BC7/BC5 when the material "
                 "loads), InferenceOnSample (decode in the material shader per sample)",
                 CVarEnum{"Off", "DecodeOnLoad", "InferenceOnSample"}, CVarFlags::Persist);
CVar<int> cvFilter("r.NeuralTextures.Filter", 0,
                   "InferenceOnSample filtering: Stochastic (1 decode per sample, TAA integrates) or Bilinear (4 decodes)",
                   CVarEnum{"Stochastic", "Bilinear"}, CVarFlags::Persist);
CVar<int> cvPrepass("r.NeuralTextures.Prepass", 1,
                    "Decode neural normals in the depth prepass too (0 = the prepass reads the BC fallback tail)",
                    S::Textures, {0, 0, 1, 1});
CVar<int> cvMinMip("r.NeuralTextures.MinMip", 0,
                   "Finest neural mip kept resident / decoded (InferenceOnSample; higher = less memory, blurrier)",
                   S::Textures, {2, 1, 0, 0});
CVar<int> cvDecodeQuality("r.NeuralTextures.DecodeQuality", 0,
                          "BC7 encoder level for DecodeOnLoad (UASTC 0 fastest .. 4); BC5 is always exhaustive",
                          S::Textures, {0, 0, 0, 1});
CVar<float> cvMipBias("r.NeuralTextures.MipBias", 0.0f, "LOD bias of neural sampling (InferenceOnSample)", -2.0f, 4.0f);
CVar<bool> cvStreaming("r.NeuralTextures.Streaming", true,
                       "Latent residency follows the texel density of the instances (InferenceOnSample)");

constexpr u32 kDropDelayFrames = 30;

// Mirrors NtcMaterial in render/neural/ntc.glsl (scalar layout).
struct GpuNeuralMaterial {
    VkDeviceAddress weights = 0;
    u32 w0 = 0, b0 = 0, w1 = 0, b1 = 0, w2 = 0, b2 = 0;
    u32 hidden = 64, width = 0, height = 0, neuralMips = 0;
    u32 minMip = 0, pairs = 0, channels = 0, features = 0;
    u32 grids[8] = {};
    u32 g0Size[4] = {};
    u32 latentSampler = 0;
    u32 flags = 0;
    f32 mipBias = 0.0f;
    u32 pad = 0;
};
static_assert(sizeof(GpuNeuralMaterial) == 128);

Uuid derived(const Uuid& set, const char* kind, u32 slot) { return Uuid::fromName(std::format("{}/ntc/{}/{}", set.toString(), kind, slot)); }

u32 packChannels(const assets::NtcChannelLayout& l) {
    auto o = [](i32 v) { return v < 0 ? 15u : u32(v); };
    return o(l.albedo) | o(l.alpha) << 4 | o(l.normal) << 8 | o(l.orm) << 12 | o(l.emissive) << 16;
}

class NeuralTexturesFeature final : public IRenderFeature, public IMaterialResolveHook {
public:
    explicit NeuralTexturesFeature(Renderer& renderer) : m_renderer(&renderer), m_device(&renderer.device()) {
        m_cache = &renderer.resources();
        m_cache->setMaterialResolveHook(this, false);
        m_mode = NeuralTextureMode(std::clamp(cvMode.get(), 0, 2));
    }
    ~NeuralTexturesFeature() override { waitJobs(); }

    std::string_view name() const override { return "NeuralTextures"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::PreDepth); }
    i32 order() const override { return -20000; }
    std::vector<std::string> cvarNames() const override {
        return {"r.NeuralTextures", "r.NeuralTextures.Filter", "r.NeuralTextures.Prepass", "r.NeuralTextures.MinMip",
                "r.NeuralTextures.DecodeQuality", "r.NeuralTextures.MipBias", "r.NeuralTextures.Streaming",
                "r.NeuralTextures.ShaderPath"};
    }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return true; }

    bool initialize(FeatureInitContext&) override { return true; }

    void shutdown(rhi::Device&) override {
        waitJobs();
        m_cache->setMaterialResolveHook(nullptr, false);
        for (auto& [id, e] : m_sets) releaseInference(e);
        m_sets.clear();
    }

    void setup(FeatureContext& ctx) override {
        if (m_device->frameNumber() != m_lastTick || !m_ticked) {
            m_lastTick = m_device->frameNumber();
            m_ticked = true;
            tick(ctx.lodSelection());
        }
        if (m_mode == NeuralTextureMode::InferenceOnSample && !m_table.empty())
            ctx.viewConstants().neuralMaterials = ctx.upload(std::span<const GpuNeuralMaterial>(m_table));
    }

    // --- IMaterialResolveHook ---
    bool resolveMaterial(const Uuid&, const assets::MaterialAsset& src, GpuMaterial& g) override {
        Entry& e = entry(src.neuralTextures);
        e.used = true;
        const bool only = src.neuralTexturesOnly;
        if (only) e.textureOnlyUsers = true;
        if (m_mode == NeuralTextureMode::Off && !only) return false;
        if (!e.set) {
            if (!only && e.state == State::Failed) return false;
            if (!only && m_mode != NeuralTextureMode::InferenceOnSample) return false; // regular textures meanwhile
            g.albedoTexture = g.normalTexture = g.ormTexture = g.emissiveTexture = kInvalidIndex;
            return true;
        }
        const bool inference = m_mode == NeuralTextureMode::InferenceOnSample && e.inferenceReady;
        u32* slots[4] = {&g.albedoTexture, &g.normalTexture, &g.ormTexture, &g.emissiveTexture};
        for (u32 i = 0; i < 4; ++i) {
            u32 index = kInvalidIndex;
            if (!inference && e.decoded) index = residentIndex(derived(e.id, "decoded", i));
            if (index == kInvalidIndex) index = residentIndex(derived(e.id, "fallback", i));
            *slots[i] = index;
        }
        if (inference) g.flags |= kMaterialNeural | (e.slot << kMaterialNeuralSlotShift);
        return true;
    }

    // --- API ---
    void addSet(const Uuid& id, std::shared_ptr<const assets::NeuralTextureSet> set) {
        Entry& e = entry(id, false);
        resetEntry(e);
        e.set = std::move(set);
        e.state = State::Ready;
        onSetLoaded(e);
        m_dirty = true;
    }
    NeuralTextureStats stats() const {
        NeuralTextureStats s = m_stats;
        s.pending = m_inFlight.load();
        return s;
    }
    void flush() {
        for (int i = 0; i < 4; ++i) {
            waitJobs();
            m_cache->flush();
            tick(m_lastCamera);
            m_cache->flush();
        }
    }

private:
    enum class State : u8 { Unloaded, Loading, Ready, Failed };
    struct Entry {
        Uuid id;
        State state = State::Unloaded;
        std::shared_ptr<const assets::NeuralTextureSet> set;
        bool used = false;
        bool textureOnlyUsers = false;
        bool fallbackRegistered = false;
        // DecodeOnLoad
        bool decoding = false;
        bool decoded = false;
        // InferenceOnSample
        bool inferenceReady = false;
        u32 slot = 0;
        NeuralNetwork network;
        std::array<rhi::TextureHandle, 8> grids{};
        u32 residentFirstPair = 0;
        u32 wantedFirstPair = 0;
        u32 dropFrames = 0;
        u64 latentBytes = 0;
        u64 decodedBytes = 0;
    };
    struct Completed {
        Uuid id;
        std::shared_ptr<const assets::NeuralTextureSet> set;
        std::shared_ptr<assets::NtcDecodedTextures> decoded;
        bool load = true;
    };

    u32 residentIndex(const Uuid& id) const {
        const GpuTexture* t = m_cache->residentTexture(id);
        return t ? t->sampledIndex : kInvalidIndex;
    }

    Entry& entry(const Uuid& id, bool load = true) {
        auto [it, inserted] = m_sets.try_emplace(id);
        Entry& e = it->second;
        if (inserted) e.id = id;
        if (load && e.state == State::Unloaded) requestLoad(e);
        return e;
    }

    void requestLoad(Entry& e) {
        const AssetProvider& provider = m_cache->provider();
        if (!provider.loadNeuralTextures) {
            e.state = State::Failed;
            return;
        }
        e.state = State::Loading;
        ++m_inFlight;
        auto job = [this, id = e.id, load = provider.loadNeuralTextures] {
            Completed c;
            c.id = id;
            c.set = load(id);
            std::lock_guard lock(m_mutex);
            m_completed.push_back(std::move(c));
            --m_inFlight;
        };
        if (JobSystem* jobs = m_cache->jobs()) jobs->submit(std::move(job));
        else job();
    }

    void startDecode(Entry& e) {
        if (e.decoding || e.decoded || !e.set) return;
        e.decoding = true;
        ++m_inFlight;
        auto job = [this, id = e.id, set = e.set, quality = u32(std::clamp(cvDecodeQuality.get(), 0, 4))] {
            Stopwatch sw(true);
            auto dec = std::make_shared<assets::NtcDecodedTextures>(assets::decodeToTextures(*set, quality));
            m_lastDecodeMs.store(sw.elapsedMs());
            Completed c;
            c.id = id;
            c.decoded = std::move(dec);
            c.load = false;
            std::lock_guard lock(m_mutex);
            m_completed.push_back(std::move(c));
            --m_inFlight;
        };
        if (JobSystem* jobs = m_cache->jobs()) jobs->submit(std::move(job));
        else job();
    }

    void waitJobs() {
        while (m_inFlight.load() > 0) std::this_thread::yield();
    }

    void onSetLoaded(Entry& e) {
        if (e.fallbackRegistered || !e.set) return;
        for (u32 i = 0; i < 4; ++i)
            if (!e.set->fallback[i].mips.empty()) m_cache->addTexture(derived(e.id, "fallback", i), e.set->fallback[i]);
        e.fallbackRegistered = true;
    }

    void releaseDecoded(Entry& e) {
        if (e.decoded) {
            for (u32 i = 0; i < 4; ++i) m_cache->remove(derived(e.id, "decoded", i));
        }
        e.decoded = false;
        e.decodedBytes = 0;
    }

    void releaseInference(Entry& e) {
        e.network.release();
        for (rhi::TextureHandle& t : e.grids) {
            if (t) m_device->destroy(t);
            t = {};
        }
        if (e.inferenceReady) m_freeSlots.push_back(e.slot);
        e.inferenceReady = false;
        e.latentBytes = 0;
    }

    void resetEntry(Entry& e) {
        releaseInference(e);
        releaseDecoded(e);
        if (e.fallbackRegistered)
            for (u32 i = 0; i < 4; ++i) m_cache->remove(derived(e.id, "fallback", i));
        e.fallbackRegistered = false;
        e.set.reset();
        e.state = State::Unloaded;
    }

    // Latent pairs [firstPair, pairs) resident; network uploaded on first use.
    void makeResident(Entry& e, u32 firstPair) {
        const assets::NeuralTextureSet& set = *e.set;
        const u32 pairs = u32(set.pairs.size());
        firstPair = std::min(firstPair, pairs ? pairs - 1 : 0);
        if (!e.network.valid() && !e.network.upload(*m_device, set.network, "ntc.weights")) return;
        e.latentBytes = 0;
        for (u32 p = 0; p < pairs; ++p) {
            for (u32 g = 0; g < 2; ++g) {
                rhi::TextureHandle& t = e.grids[p * 2 + g];
                if (p < firstPair) {
                    if (t) m_device->destroy(t);
                    t = {};
                } else if (!t) {
                    t = detail::uploadLatentGrid(*m_device, set, set.pairs[p].grids[g], true);
                }
                if (t) e.latentBytes += rhi::estimateTextureSize(m_device->desc(t));
            }
        }
        e.residentFirstPair = firstPair;
        if (!e.inferenceReady) {
            if (m_freeSlots.empty()) m_freeSlots.push_back(m_nextSlot++);
            e.slot = m_freeSlots.back();
            m_freeSlots.pop_back();
            e.inferenceReady = true;
        }
    }

    void transition(NeuralTextureMode from, NeuralTextureMode to) {
        OX_LOG_INFO("render", "r.NeuralTextures: {} -> {}", neuralTextureModeName(from), neuralTextureModeName(to));
        // Cache removals refresh every material (and may add entries): collect first, release after the loop.
        std::vector<Entry*> undecode;
        for (auto& [id, e] : m_sets) {
            if (to != NeuralTextureMode::InferenceOnSample) releaseInference(e);
            const bool needDecoded = to == NeuralTextureMode::DecodeOnLoad || (to == NeuralTextureMode::Off && e.textureOnlyUsers);
            if (!needDecoded) undecode.push_back(&e);
        }
        for (Entry* e : undecode) releaseDecoded(*e);
        m_dirty = true;
    }

    void updateResidency(const LodSelection& camera) {
        // Projected size per GPU material (max over visible instances), like the texture streamer.
        const GpuScene& scene = m_renderer->scene();
        const GpuMaterial* materials = scene.materials();
        GpuSceneHeader header;
        scene.fillHeader(header);
        m_materialPixels.assign(header.materialCount, 0.0f);
        for (const GpuInstance& inst : scene.instances()) {
            if (!(inst.flags & kInstanceVisible) || inst.materialIndex >= header.materialCount) continue;
            const glm::vec3 c(inst.boundingSphere);
            const f32 r = inst.boundingSphere.w;
            const f32 dist = std::max(glm::length(c - camera.cameraPosition) - r, 0.05f);
            const f32 px = camera.orthographic ? 2.0f * r * camera.projScale : 2.0f * r * camera.projScale / dist;
            m_materialPixels[inst.materialIndex] = std::max(m_materialPixels[inst.materialIndex], px);
        }
        std::unordered_map<u32, Entry*> bySlot;
        for (auto& [id, e] : m_sets) {
            if (e.inferenceReady) bySlot[e.slot] = &e;
            e.wantedFirstPair = e.set ? u32(e.set->pairs.size()) : 0;
        }
        for (u32 m = 0; m < m_materialPixels.size(); ++m) {
            const GpuMaterial& mat = materials[m];
            if (!(mat.flags & kMaterialNeural) || m_materialPixels[m] <= 0.0f) continue;
            auto it = bySlot.find(mat.flags >> kMaterialNeuralSlotShift);
            if (it == bySlot.end()) continue;
            Entry& e = *it->second;
            const f32 tiling = std::max({std::abs(mat.uvTiling.x), std::abs(mat.uvTiling.y), 1e-3f});
            const f32 size = f32(std::max(e.set->width, e.set->height));
            const f32 level = std::log2(std::max(size / (m_materialPixels[m] * tiling), 1.0f)) + cvMipBias.get();
            e.wantedFirstPair = std::min(e.wantedFirstPair, u32(std::max(level, 0.0f)) / 2);
        }
    }

    void tick(const LodSelection& camera) {
        OX_PROFILE_ZONE();
        m_lastCamera = camera;
        const NeuralTextureMode mode = NeuralTextureMode(std::clamp(cvMode.get(), 0, 2));
        if (mode != m_mode) {
            transition(m_mode, mode);
            m_mode = mode;
        }
        // Finished loads / decodes.
        std::vector<Completed> done;
        {
            std::lock_guard lock(m_mutex);
            done.swap(m_completed);
        }
        for (Completed& c : done) {
            auto it = m_sets.find(c.id);
            if (it == m_sets.end()) continue;
            Entry& e = it->second;
            if (c.load) {
                if (c.set) {
                    e.set = std::move(c.set);
                    e.state = State::Ready;
                    onSetLoaded(e);
                } else {
                    e.state = State::Failed;
                    OX_LOG_WARN("render", "neural texture set {} failed to load", c.id.toString());
                }
            } else {
                e.decoding = false;
                const bool want = m_mode == NeuralTextureMode::DecodeOnLoad || (m_mode == NeuralTextureMode::Off && e.textureOnlyUsers);
                if (want && c.decoded) {
                    e.decodedBytes = 0;
                    for (u32 i = 0; i < 4; ++i) {
                        const assets::TextureData& t = c.decoded->textures[i];
                        if (t.mips.empty()) continue;
                        m_cache->addTexture(derived(e.id, "decoded", i), t);
                        for (const auto& m : t.mips) e.decodedBytes += m.data.size();
                    }
                    e.decoded = true;
                }
            }
            m_dirty = true;
        }
        // Mode-specific resources.
        const u32 minPair = u32(std::max(cvMinMip.get(), 0)) / 2;
        if (m_mode == NeuralTextureMode::InferenceOnSample && cvStreaming) updateResidency(camera);
        for (auto& [id, e] : m_sets) {
            if (!e.set || !e.used) continue;
            const bool needDecoded = m_mode == NeuralTextureMode::DecodeOnLoad || (m_mode == NeuralTextureMode::Off && e.textureOnlyUsers);
            if (needDecoded) startDecode(e);
            if (m_mode != NeuralTextureMode::InferenceOnSample) continue;
            const u32 pairs = u32(e.set->pairs.size());
            u32 wanted = std::max(cvStreaming ? e.wantedFirstPair : 0u, minPair);
            wanted = std::min(wanted, pairs ? pairs - 1 : 0);
            if (!e.inferenceReady) {
                makeResident(e, wanted);
                m_dirty = true;
            } else if (wanted < e.residentFirstPair) {
                makeResident(e, wanted);
                e.dropFrames = 0;
            } else if (wanted > e.residentFirstPair) {
                if (++e.dropFrames >= kDropDelayFrames) {
                    makeResident(e, wanted);
                    e.dropFrames = 0;
                }
            } else {
                e.dropFrames = 0;
            }
        }
        if (m_dirty) {
            m_dirty = false;
            m_cache->refreshMaterials();
        }
        // Table + stats.
        m_table.assign(m_nextSlot, GpuNeuralMaterial{});
        NeuralTextureStats st;
        const u32 flags = (cvFilter.get() == 1 ? 1u : 0u) | (cvPrepass.get() != 0 ? 2u : 0u);
        for (auto& [id, e] : m_sets) {
            if (!e.set) continue;
            ++st.materials;
            st.decodedBytes += e.decodedBytes;
            if (e.decoded) ++st.decodedMaterials;
            if (!e.inferenceReady) continue;
            ++st.inferenceMaterials;
            st.latentBytes += e.latentBytes;
            st.weightBytes += e.network.bytes();
            st.fallbackBytes += e.set->fallbackBytes();
            st.latentPairsTotal += u32(e.set->pairs.size());
            st.latentPairsResident += u32(e.set->pairs.size()) - e.residentFirstPair;
            const auto& l = e.network.layers();
            GpuNeuralMaterial& g = m_table[e.slot];
            g.weights = e.network.address();
            g.w0 = l[0].weightOffset, g.b0 = l[0].biasOffset;
            g.w1 = l[1].weightOffset, g.b1 = l[1].biasOffset;
            g.w2 = l[2].weightOffset, g.b2 = l[2].biasOffset;
            g.hidden = e.set->hidden();
            g.width = e.set->width;
            g.height = e.set->height;
            g.neuralMips = e.set->neuralMips;
            g.minMip = std::max(e.residentFirstPair * 2, u32(std::max(cvMinMip.get(), 0)));
            g.pairs = u32(e.set->pairs.size());
            g.channels = packChannels(e.set->channels);
            g.features = e.set->pairs.empty() ? 0 : e.set->pairs[0].grids[0].features | e.set->pairs[0].grids[1].features << 8;
            for (u32 p = 0; p < e.set->pairs.size() && p < 4; ++p) {
                for (u32 k = 0; k < 2; ++k) g.grids[p * 2 + k] = e.grids[p * 2 + k] ? m_device->sampledIndex(e.grids[p * 2 + k]) : kInvalidIndex;
                g.g0Size[p] = e.set->pairs[p].grids[0].width | e.set->pairs[p].grids[0].height << 16;
            }
            g.latentSampler = 0; // DefaultSampler::LinearRepeat
            g.flags = flags;
            g.mipBias = cvMipBias.get();
        }
        st.lastDecodeMs = m_lastDecodeMs.load();
        m_stats = st;
    }

    Renderer* m_renderer;
    rhi::Device* m_device;
    GpuResourceCache* m_cache;
    NeuralTextureMode m_mode = NeuralTextureMode::Off;
    std::unordered_map<Uuid, Entry> m_sets;
    std::vector<GpuNeuralMaterial> m_table;
    std::vector<u32> m_freeSlots;
    u32 m_nextSlot = 0;
    std::vector<f32> m_materialPixels;
    std::mutex m_mutex;
    std::vector<Completed> m_completed;
    std::atomic<u32> m_inFlight{0};
    std::atomic<f64> m_lastDecodeMs{0.0};
    NeuralTextureStats m_stats;
    LodSelection m_lastCamera;
    u64 m_lastTick = 0;
    bool m_ticked = false;
    bool m_dirty = false;
};

NeuralTexturesFeature* findFeature(Renderer& r) {
    return dynamic_cast<NeuralTexturesFeature*>(r.features().find("NeuralTextures"));
}

} // namespace

NeuralTextureMode neuralTextureMode() { return NeuralTextureMode(std::clamp(cvMode.get(), 0, 2)); }

const char* neuralTextureModeName(NeuralTextureMode m) {
    switch (m) {
    case NeuralTextureMode::Off: return "Off";
    case NeuralTextureMode::DecodeOnLoad: return "DecodeOnLoad";
    case NeuralTextureMode::InferenceOnSample: return "InferenceOnSample";
    }
    return "?";
}

void registerNeuralTextureFeature(Renderer& renderer) { renderer.features().add(std::make_unique<NeuralTexturesFeature>(renderer)); }

NeuralTextureStats neuralTextureStats(Renderer& renderer) {
    NeuralTexturesFeature* f = findFeature(renderer);
    return f ? f->stats() : NeuralTextureStats{};
}

void addNeuralTextureSet(Renderer& renderer, const Uuid& id, std::shared_ptr<const assets::NeuralTextureSet> set) {
    if (NeuralTexturesFeature* f = findFeature(renderer)) f->addSet(id, std::move(set));
}

void flushNeuralTextures(Renderer& renderer) {
    if (NeuralTexturesFeature* f = findFeature(renderer)) f->flush();
}

} // namespace ox::render::neural
