// "Particles": GPU particle systems. Simulation (compute, persistent buffers with dead / alive lists and GPU-written
// indirect draw args — MoltenVK has no drawIndirectCount) once per frame in the first view; rendering of billboards,
// stretched billboards and meshes into a low-resolution buffer with soft particles and depth-aware upsampling.
#include "translucency_internal.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/render/mesh_primitives.hpp>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <unordered_map>

namespace ox::render {

namespace {

using S = Scalability;

CVar<bool> cvParticles("r.Particles", true, "GPU particles");
CVar<int> cvBudget("r.Particles.Budget", 262144, "Maximum particles per emitter (clamps maxParticles)", S::Effects,
                   {16384, 65536, 262144, 1048576});
CVar<int> cvDivisor("r.Particles.ResolutionDivisor", 2,
                    "Particle buffer resolution: 1 full, 2 half, 4 quarter (depth-aware upsampling)", S::Effects,
                    {4, 2, 2, 1});
CVar<bool> cvCollision("r.Particles.Collision", true, "Depth buffer collisions", S::Effects, {false, true, true, true});
CVar<bool> cvLighting("r.Particles.Lighting", true, "Lit particles (clusters, sun shadow, ambient)", S::Effects,
                      {false, true, true, true});
CVar<bool> cvSorting("r.Particles.Sorting", true, "Back-to-front sorting for emitters that request it", S::Effects,
                     {false, false, true, true});
CVar<bool> cvSoft("r.Particles.SoftParticles", true, "Soft particle depth fade", S::Effects, {false, true, true, true});

struct ParticleGpu {
    glm::vec3 position;
    f32 age;
    glm::vec3 velocity;
    f32 lifetime;
    f32 size, rotation, rotationSpeed, frameOffset;
    u32 seed, pad0;
    f32 pad1, pad2;
};
static_assert(sizeof(ParticleGpu) == 64);

struct EmitterGpu {
    glm::mat4 world{1.0f};
    glm::vec4 shape{0.0f}, box{0.0f}, lifetime{0.0f}, velocity{0.0f}, gravity{0.0f}, turbulence{0.0f}, size{0.0f},
        rotation{0.0f}, color{1.0f}, flipbook{1.0f, 1.0f, 0.0f, 0.0f}, collision{0.0f};
    u32 maxParticles = 0, emitCount = 0, seed = 0, current = 0;
    u32 meshFirstIndex = 0, meshTriangles = 0;
    i32 meshVertexOffset = 0;
    u32 flags = 0;
    u32 gradient = kInvalidIndex, texture = kInvalidIndex, meshIndexCount = 0, pad = 0;
};
static_assert(sizeof(EmitterGpu) == 64 + 11 * 16 + 12 * 4);

struct SimPush {
    u64 view = 0, scene = 0, emitter = 0, particles = 0, lists = 0, counters = 0, indices = 0;
    u32 mode = 0;
    u32 sceneDepth = kInvalidIndex;
    u32 normals = kInvalidIndex;
    u32 sortCount = 0, sortK = 0, sortJ = 0;
};
static_assert(sizeof(SimPush) == 80);

struct RenderPush {
    u64 view = 0, scene = 0, emitter = 0, particles = 0, lists = 0;
    u32 listOffset = 0, listStride = 1, listSlot = 0;
    u32 depth = kInvalidIndex;
    u32 fog = kInvalidIndex;
    u32 lowRes = 0;
    glm::vec2 targetSize{0.0f};
};
static_assert(sizeof(RenderPush) == 72);

constexpr u64 kCountersSize = 64;
constexpr u32 kMaxSortedParticles = 2048; // particles_sort.comp: one workgroup, shared memory
constexpr u64 kDrawOffset = 16;
constexpr u64 kDrawIndexedOffset = 32;

u32 nextPow2(u32 v) { return v <= 1 ? 1 : std::bit_ceil(v); }

// Piecewise linear curve sampling (keys sorted by time, empty = default).
f32 sampleCurve(const std::vector<ParticleCurveKey>& keys, f32 t, f32 def) {
    if (keys.empty()) return def;
    if (t <= keys.front().time) return keys.front().value;
    for (usize i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].time) {
            const f32 span = std::max(keys[i].time - keys[i - 1].time, 1e-6f);
            return glm::mix(keys[i - 1].value, keys[i].value, (t - keys[i - 1].time) / span);
        }
    }
    return keys.back().value;
}

glm::vec4 sampleColor(const std::vector<ParticleColorKey>& keys, f32 t) {
    if (keys.empty()) return glm::vec4(1.0f);
    if (t <= keys.front().time) return keys.front().color;
    for (usize i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].time) {
            const f32 span = std::max(keys[i].time - keys[i - 1].time, 1e-6f);
            return glm::mix(keys[i - 1].color, keys[i].color, (t - keys[i - 1].time) / span);
        }
    }
    return keys.back().color;
}

u64 curvesHash(const ParticleEmitterComponent& e) {
    u64 h = 1469598103934665603ull;
    auto mix = [&](const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    for (const auto& k : e.sizeOverLife) mix(&k, sizeof(k));
    for (const auto& k : e.colorOverLife) mix(&k, sizeof(k));
    mix(&h, 1);
    return h;
}

// --- procedural sprites -------------------------------------------------------------------------------------

f32 hashf(i32 x, i32 y, u32 s) {
    u32 h = u32(x) * 374761393u + u32(y) * 668265263u + s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return f32((h ^ (h >> 16)) & 0xffffffu) / f32(0xffffff);
}

f32 vnoise(f32 x, f32 y, u32 s) {
    const i32 x0 = i32(std::floor(x)), y0 = i32(std::floor(y));
    const f32 fx = x - f32(x0), fy = y - f32(y0);
    const f32 sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const f32 a = glm::mix(hashf(x0, y0, s), hashf(x0 + 1, y0, s), sx);
    const f32 b = glm::mix(hashf(x0, y0 + 1, s), hashf(x0 + 1, y0 + 1, s), sx);
    return glm::mix(a, b, sy);
}

std::vector<u8> makeSoftCircle(u32 n, bool spark) {
    std::vector<u8> px(usize(n) * n * 4);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const f32 u = (f32(x) + 0.5f) / f32(n) * 2.0f - 1.0f, v = (f32(y) + 0.5f) / f32(n) * 2.0f - 1.0f;
            f32 a;
            if (spark) {
                const f32 r = std::sqrt(u * u * 4.0f + v * v * 0.35f);
                a = std::clamp(1.0f - r, 0.0f, 1.0f);
                a = a * a * (0.6f + 0.4f * std::exp(-r * 6.0f));
            } else {
                const f32 r = std::sqrt(u * u + v * v);
                a = std::clamp(1.0f - r, 0.0f, 1.0f);
                a = a * a * (3.0f - 2.0f * a);
            }
            const usize i = (usize(y) * n + x) * 4;
            px[i] = px[i + 1] = px[i + 2] = 255;
            px[i + 3] = u8(a * 255.0f + 0.5f);
        }
    }
    return px;
}

// 4×4 flipbook of a billowing smoke puff (fbm noise, radial falloff, expanding and thinning over the frames).
std::vector<u8> makeSmokeAtlas(u32 cell) {
    const u32 n = cell * 4;
    std::vector<u8> px(usize(n) * n * 4);
    for (u32 f = 0; f < 16; ++f) {
        const f32 t = f32(f) / 15.0f;
        const u32 ox = (f % 4) * cell, oy = (f / 4) * cell;
        for (u32 y = 0; y < cell; ++y) {
            for (u32 x = 0; x < cell; ++x) {
                const f32 u = (f32(x) + 0.5f) / f32(cell) * 2.0f - 1.0f, v = (f32(y) + 0.5f) / f32(cell) * 2.0f - 1.0f;
                const f32 r = std::sqrt(u * u + v * v) / (0.75f + 0.25f * t);
                f32 noise = 0.0f, amp = 0.5f, fr = 3.0f;
                for (u32 o = 0; o < 4; ++o) {
                    noise += vnoise(u * fr + t * 1.5f + f32(o) * 7.1f, v * fr - t * 0.8f, 3 + o) * amp;
                    amp *= 0.5f;
                    fr *= 2.0f;
                }
                f32 a = std::clamp(1.0f - r, 0.0f, 1.0f);
                a = a * a * std::clamp(noise * 1.6f - 0.15f - 0.25f * r, 0.0f, 1.0f) * (1.0f - 0.35f * t);
                const f32 shade = 0.75f + 0.25f * std::clamp(noise * 1.2f - v * 0.3f, 0.0f, 1.0f);
                const usize i = (usize(oy + y) * n + ox + x) * 4;
                px[i] = px[i + 1] = px[i + 2] = u8(std::clamp(shade, 0.0f, 1.0f) * 255.0f + 0.5f);
                px[i + 3] = u8(std::clamp(a * 1.4f, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    }
    return px;
}

// --- per emitter GPU state ----------------------------------------------------------------------------------

struct EmitterState {
    rhi::BufferHandle particles, lists, counters;
    rhi::TextureHandle gradient;
    u64 gradientHash = 0;
    u32 capacity = 0;
    u32 sortCount = 0;
    u32 current = 0;
    bool needsInit = true;
    f32 localTime = 0.0f;
    f32 spawnAccum = 0.0f;
    bool finished = false;
    bool lastSorted = false; // the simulation wrote sorted (key, slot) pairs this frame
    std::vector<u32> burstFired;
    u64 lastFrame = 0;
    u32 frameCounter = 0;

    void release(rhi::Device& dev) {
        for (auto b : {particles, lists, counters}) {
            if (b) dev.destroy(b);
        }
        if (gradient) dev.destroy(gradient);
        particles = lists = counters = {};
        gradient = {};
    }
};

class ParticlesFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "Particles"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::AfterOpaque, InjectionPoint::Translucency); }
    i32 order() const override { return 100; } // after the refraction/depth copies and transparent objects
    std::vector<std::string> cvarNames() const override {
        return {"r.Particles", "r.Particles.Budget", "r.Particles.ResolutionDivisor", "r.Particles.Collision",
                "r.Particles.Lighting", "r.Particles.Sorting", "r.Particles.SoftParticles"};
    }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cvParticles; }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::Device& dev = ctx.device;
        m_sim = createComputePipeline(dev, "particles.simulate", "render/translucency/particles_sim.comp");
        m_sort = createComputePipeline(dev, "particles.sort", "render/translucency/particles_sort.comp");
        m_lowDepth = createComputePipeline(dev, "particles.lowResDepth", "render/translucency/particles_lowres.comp");
        m_source = translucency::createSourcePipeline(dev);
        for (u32 mesh = 0; mesh < 2; ++mesh) {
            rhi::GraphicsPipelineDesc d;
            d.name = mesh ? "particles.mesh" : "particles.billboard";
            std::vector<rhi::ShaderDefine> defs = translucency::contractDefines();
            if (mesh) defs.push_back({"OX_PARTICLE_MESH"});
            d.vertex = rhi::ShaderStageDesc::file("render/translucency/particle.vert", defs);
            d.fragment = rhi::ShaderStageDesc::file("render/translucency/particle.frag", defs);
            d.colorFormats = {formats::kSceneColor};
            d.blend = {rhi::BlendState::premultiplied()};
            d.raster.cullMode = VK_CULL_MODE_NONE;
            m_draw[mesh] = dev.createGraphicsPipeline(d);
        }
        m_composite = createFullscreenPipeline(dev, "particles.composite", "render/translucency/particles_composite.frag",
                                               {formats::kSceneColor}, {rhi::BlendState::premultiplied()});
        m_sprites[0] = translucency::createTexture2D(dev, "particles.softCircle", 64, 64, makeSoftCircle(64, false));
        m_sprites[1] = translucency::createTexture2D(dev, "particles.smoke", 256, 256, makeSmokeAtlas(64));
        m_sprites[2] = translucency::createTexture2D(dev, "particles.spark", 64, 64, makeSoftCircle(64, true));
        return true;
    }

    void shutdown(rhi::Device& dev) override {
        for (auto& [id, s] : m_states) s.release(dev);
        m_states.clear();
        for (auto p : {m_sim, m_sort, m_lowDepth, m_source, m_draw[0], m_draw[1], m_composite}) dev.destroy(p);
        for (auto t : m_sprites) dev.destroy(t);
    }

    void setup(FeatureContext& ctx) override {
        const TranslucencySnapshot* data = ctx.snapshot().findExtension<TranslucencySnapshot>();
        if (!data || data->emitters.empty()) {
            if (ctx.point() == InjectionPoint::AfterOpaque && !m_states.empty() && frameState(ctx).firstViewOfFrame) {
                for (auto& [id, s] : m_states) s.release(ctx.device());
                m_states.clear();
            }
            return;
        }
        if (ctx.point() == InjectionPoint::AfterOpaque) simulate(ctx, *data);
        else draw(ctx, *data);
    }

private:
    struct Prepared {
        u32 entityId = 0;
        EmitterState* state = nullptr;
        EmitterGpu gpu;
        VkDeviceAddress emitterAddress = 0;
        u32 emitCount = 0;
        bool sort = false;
        bool mesh = false;
    };

    void ensureBuffers(rhi::Device& dev, EmitterState& s, u32 capacity) {
        if (s.capacity == capacity && s.particles) return;
        s.release(dev);
        s.capacity = capacity;
        s.sortCount = nextPow2(capacity);
        s.particles = dev.createBuffer({u64(capacity) * sizeof(ParticleGpu), rhi::BufferUsage::Storage,
                                        rhi::MemoryUsage::GpuOnly, "particles.data"});
        s.lists = dev.createBuffer({(u64(capacity) * 3 + u64(s.sortCount)) * 4, rhi::BufferUsage::Storage,
                                    rhi::MemoryUsage::GpuOnly, "particles.lists"});
        s.counters = dev.createBuffer({kCountersSize, rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect,
                                       rhi::MemoryUsage::GpuOnly, "particles.counters"});
        s.needsInit = true;
        s.current = 0;
        s.gradientHash = 0;
    }

    void updateGradient(rhi::Device& dev, EmitterState& s, const ParticleEmitterComponent& e) {
        const u64 h = curvesHash(e);
        if (s.gradient && s.gradientHash == h) return;
        if (!s.gradient) {
            rhi::TextureDesc d;
            d.name = "particles.gradient";
            d.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            d.width = 64;
            d.height = 2;
            d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
            s.gradient = dev.createTexture(d);
        }
        std::vector<u16> texels(64 * 2 * 4);
        for (u32 x = 0; x < 64; ++x) {
            const f32 t = f32(x) / 63.0f;
            const glm::vec4 c = sampleColor(e.colorOverLife, t);
            const f32 sz = sampleCurve(e.sizeOverLife, t, 1.0f);
            for (u32 ch = 0; ch < 4; ++ch) texels[x * 4 + ch] = glm::packHalf1x16(c[ch]);
            for (u32 ch = 0; ch < 4; ++ch) texels[(64 + x) * 4 + ch] = glm::packHalf1x16(ch == 0 ? sz : 1.0f);
        }
        dev.uploadTextureAsync(s.gradient, {reinterpret_cast<const u8*>(texels.data()), texels.size() * 2});
        s.gradientHash = h;
    }

    // Spawn count for this frame (rate + bursts), advancing the emitter clock.
    static u32 spawnCount(EmitterState& s, const ParticleEmitterComponent& e, f32 dt) {
        if (s.burstFired.size() != e.bursts.size()) s.burstFired.assign(e.bursts.size(), 0);
        if (dt <= 0.0f || s.finished) return 0;
        const f32 duration = std::max(e.duration, 0.01f);
        const f32 t0 = s.localTime, t1 = t0 + dt;
        f64 count = 0.0;
        const f32 rateEnd = e.loop ? t1 : std::min(t1, duration);
        if (rateEnd > t0) {
            s.spawnAccum += e.spawnRate * (rateEnd - t0);
            const f32 n = std::floor(s.spawnAccum);
            s.spawnAccum -= n;
            count += n;
        }
        for (usize b = 0; b < e.bursts.size(); ++b) {
            const ParticleBurst& bu = e.bursts[b];
            for (u32 guard = 0; guard < 1024; ++guard) {
                const u32 c = s.burstFired[b];
                if (bu.cycles != 0 && c >= bu.cycles) break;
                const f32 fire = bu.time + f32(c) * std::max(bu.interval, 1e-3f);
                if (fire >= t1 || (!e.loop && fire > duration)) break;
                count += bu.count;
                s.burstFired[b] = c + 1;
            }
        }
        if (t1 >= duration) {
            if (e.loop) {
                s.localTime = std::fmod(t1, duration);
                std::fill(s.burstFired.begin(), s.burstFired.end(), 0u);
            } else {
                s.localTime = t1;
                s.finished = true;
            }
        } else {
            s.localTime = t1;
        }
        return u32(std::min<f64>(count, 1e7));
    }

    std::vector<Prepared> prepare(FeatureContext& ctx, const TranslucencySnapshot& data, bool advance) {
        rhi::Device& dev = ctx.device();
        GpuResourceCache& cache = ctx.renderer().resources();
        GpuScene& scene = ctx.scene();
        const f32 dt = std::clamp(ctx.snapshot().deltaTime, 0.0f, 0.1f);
        const u32 budget = u32(std::max(i32(cvBudget), 64));
        std::vector<Prepared> out;
        for (const SnapshotParticleEmitter& se : data.emitters) {
            const ParticleEmitterComponent& e = se.emitter;
            Prepared p;
            p.entityId = se.entityId;
            auto it = m_states.find(se.entityId);
            if (it == m_states.end()) {
                if (!advance) continue; // not simulated yet (first view creates it)
                it = m_states.emplace(se.entityId, EmitterState{}).first;
            }
            EmitterState& s = it->second;
            p.state = &s;
            const u32 capacity = std::clamp(e.maxParticles, 1u, budget);
            if (advance) {
                ensureBuffers(dev, s, capacity);
                updateGradient(dev, s, e);
                p.emitCount = std::min(spawnCount(s, e, dt), capacity);
                s.lastFrame = m_frame;
                s.frameCounter += 1;
            } else if (!s.particles) {
                continue;
            }
            EmitterGpu& g = p.gpu;
            g.world = se.world;
            g.shape = {f32(u32(e.shape)), e.radius, glm::radians(std::clamp(e.coneAngle, 0.0f, 180.0f)),
                       e.emitFromShell ? 1.0f : 0.0f};
            g.box = {e.boxExtents, 0.0f};
            g.lifetime = {std::min(e.lifetime.x, e.lifetime.y), std::max(e.lifetime.x, e.lifetime.y), e.speed.x, e.speed.y};
            g.velocity = {glm::mat3(se.world) * e.velocity, std::max(e.drag, 0.0f)};
            g.gravity = {e.gravity * e.gravityScale, std::max(e.turbulence, 0.0f)};
            g.turbulence = {e.turbulenceFrequency, e.turbulenceSpeed, f32(ctx.snapshot().time) + f32(s.frameCounter) * dt, dt};
            g.size = {e.size.x, e.size.y, glm::radians(e.rotation.x), glm::radians(e.rotation.y)};
            g.rotation = {glm::radians(e.rotationSpeed.x), glm::radians(e.rotationSpeed.y), e.stretch,
                          cvSoft ? e.softDistance : 0.0f};
            g.color = e.color;
            u32 cols = std::max(e.atlasColumns, 1u), rows = std::max(e.atlasRows, 1u);
            g.texture = kInvalidIndex;
            if (e.texture.isValid()) {
                if (const GpuTexture* t = cache.texture(e.texture); t && t->texture) g.texture = t->sampledIndex;
            } else {
                g.texture = dev.sampledIndex(m_sprites[u32(e.sprite) % 3]);
                if (e.sprite == ParticleSprite::Smoke) cols = rows = 4;
                else cols = rows = 1;
            }
            g.flipbook = {f32(cols), f32(rows), e.flipbookFps, e.randomStartFrame ? 1.0f : 0.0f};
            g.collision = {cvCollision ? f32(u32(e.collision)) : 0.0f, e.bounce, e.friction, e.emissive};
            g.maxParticles = capacity;
            g.emitCount = p.emitCount;
            g.seed = u32(se.entityId * 2654435761u) ^ (e.seed * 40503u) ^ (s.frameCounter * 2246822519u);
            g.current = s.current;
            p.sort = e.sort && cvSorting && capacity <= kMaxSortedParticles;
            g.flags = ((e.lit && cvLighting) ? 1u : 0u) | (u32(e.blend) << 1) | (u32(e.renderMode) << 3) | (p.sort ? 32u : 0u);
            g.gradient = s.gradient ? dev.sampledIndex(s.gradient) : kInvalidIndex;
            auto meshInfo = [&](const Uuid& id, GpuMeshInfo& out) {
                if (!id.isValid()) return false;
                const GpuMesh* m = cache.mesh(id);
                if (!m || m->submeshCount == 0) return false;
                out = scene.meshInfo(m->firstMeshInfo);
                return true;
            };
            GpuMeshInfo mi;
            if (e.shape == ParticleShape::MeshSurface && meshInfo(e.shapeMesh, mi)) {
                g.meshFirstIndex = mi.firstIndex;
                g.meshTriangles = mi.indexCount / 3;
                g.meshVertexOffset = mi.vertexOffset;
            }
            if (e.renderMode == ParticleRenderMode::Mesh) {
                const Uuid meshId = e.mesh.isValid() ? e.mesh : primitiveUuid(Primitive::Cube);
                if (meshInfo(meshId, mi)) {
                    g.meshFirstIndex = mi.firstIndex;
                    g.meshIndexCount = mi.indexCount;
                    g.meshVertexOffset = mi.vertexOffset;
                    p.mesh = true;
                } else {
                    continue; // mesh still loading
                }
            }
            p.emitterAddress = ctx.upload(std::span<const EmitterGpu>(&g, 1));
            out.push_back(p);
        }
        return out;
    }

    void simulate(FeatureContext& ctx, const TranslucencySnapshot& data) {
        OX_PROFILE_ZONE();
        if (!frameState(ctx).firstViewOfFrame) return; // simulate once per frame
        ++m_frame;
        m_imports.clear();
        std::vector<Prepared> list = prepare(ctx, data, true);
        // Drop emitters that disappeared.
        for (auto it = m_states.begin(); it != m_states.end();) {
            if (it->second.lastFrame != m_frame) {
                it->second.release(ctx.device());
                it = m_states.erase(it);
            } else {
                ++it;
            }
        }
        if (list.empty()) return;
        rhi::Device& dev = ctx.device();
        FrameResources& R = ctx.resources();
        const translucency::RefractionSources src = translucency::ensureSources(ctx, m_source);
        const rhi::RGTexture normals = R.texture(res::kNormals);
        const bool collide = cvCollision;
        rhi::PassBuilder pb = ctx.graph().addPass("Particles.Simulate", rhi::PassType::Compute);
        if (collide) {
            pb.read(src.depth, rhi::Access::SampledCompute);
            if (normals.valid()) pb.read(normals, rhi::Access::SampledCompute);
        }
        struct Job {
            Prepared p;
            rhi::RGBuffer particles, lists, counters;
            bool init = false;
            u32 sortCount = 0;
        };
        std::vector<Job> jobs;
        for (const Prepared& p : list) {
            Job j{p, importBuffer(ctx, p.state->particles), importBuffer(ctx, p.state->lists),
                  importBuffer(ctx, p.state->counters), p.state->needsInit, p.state->sortCount};
            m_imports[p.entityId] = {j.particles, j.lists, j.counters};
            pb.write(j.particles, rhi::Access::StorageWriteCompute)
                .write(j.lists, rhi::Access::StorageWriteCompute)
                .write(j.counters, rhi::Access::StorageWriteCompute);
            jobs.push_back(j);
        }
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        const VkDeviceAddress indices = dev.address(ctx.scene().indexBuffer());
        pb.execute([=, this](rhi::PassContext& pcx) {
            rhi::CommandList& cmd = pcx.cmd;
            cmd.bindPipeline(m_sim);
            for (const Job& j : jobs) {
                SimPush pc;
                pc.view = view, pc.scene = scene, pc.emitter = j.p.emitterAddress, pc.indices = indices;
                pc.particles = pcx.address(j.particles);
                pc.lists = pcx.address(j.lists);
                pc.counters = pcx.address(j.counters);
                pc.sceneDepth = collide ? pcx.sampledIndex(src.depth) : kInvalidIndex;
                pc.normals = collide && normals.valid() ? pcx.sampledIndex(normals) : kInvalidIndex;
                const u32 cap = j.p.gpu.maxParticles;
                auto run = [&](u32 mode, u32 threads) {
                    pc.mode = mode;
                    cmd.pushConstants(pc);
                    cmd.dispatch((threads + 63) / 64);
                    cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
                };
                if (j.init) run(0, cap);
                if (j.p.emitCount > 0) run(1, j.p.emitCount);
                run(2, cap);
                run(3, 1);
                if (j.p.sort) {
                    pc.sortCount = j.sortCount;
                    cmd.bindPipeline(m_sort);
                    cmd.pushConstants(pc);
                    cmd.dispatch(1);
                    cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
                    cmd.bindPipeline(m_sim);
                }
            }
            cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::IndirectBuffer);
        });
        for (const Prepared& p : list) {
            p.state->needsInit = false;
            p.state->current ^= 1u; // the list written this frame is read next frame (and drawn now)
            p.state->lastSorted = p.sort;
        }
    }

    rhi::RGBuffer importBuffer(FeatureContext& ctx, rhi::BufferHandle b) {
        return ctx.graph().importBuffer(b, ctx.device().desc(b), {rhi::Access::General, rhi::Access::Undefined});
    }

    void draw(FeatureContext& ctx, const TranslucencySnapshot& data) {
        OX_PROFILE_ZONE();
        std::vector<Prepared> list = prepare(ctx, data, false);
        if (list.empty()) return;
        FrameResources& R = ctx.resources();
        rhi::RenderGraph& g = ctx.graph();
        const Extent2D full = ctx.renderExtent();
        const u32 divisor = std::clamp<u32>(u32(std::max(i32(cvDivisor), 1)), 1u, 8u);
        const Extent2D low{std::max(1u, (full.width + divisor - 1) / divisor), std::max(1u, (full.height + divisor - 1) / divisor)};
        const translucency::RefractionSources src = translucency::ensureSources(ctx, m_source);
        const rhi::RGTexture hdr = R.texture(res::kSceneColorHDR);
        const bool lowRes = divisor > 1;
        rhi::RGTexture target = hdr, depthForSoft = src.depth;
        if (lowRes) {
            target = g.createTexture(translucency::textureDesc(formats::kSceneColor, low, "ParticlesLowRes"));
            depthForSoft = g.createTexture(translucency::textureDesc(VK_FORMAT_R32_SFLOAT, low, "ParticlesLowResDepth"));
            g.addPass("Particles.LowResDepth", rhi::PassType::Compute)
                .read(src.depth, rhi::Access::SampledCompute)
                .overwrite(depthForSoft, rhi::Access::StorageWriteCompute)
                .execute([=, this](rhi::PassContext& p) {
                    struct {
                        u32 src, dst, scale, sw, sh, dw, dh;
                    } pc{p.sampledIndex(src.depth), p.storageIndex(depthForSoft), divisor, full.width, full.height,
                         low.width, low.height};
                    p.cmd.bindPipeline(m_lowDepth);
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatch((low.width + 7) / 8, (low.height + 7) / 8);
                });
        }
        struct Job {
            Prepared p;
            rhi::RGBuffer particles, lists, counters;
            u32 listOffset = 0, listStride = 1, listSlot = 0;
        };
        std::vector<Job> jobs;
        rhi::PassBuilder pb = g.addPass("Particles.Render");
        if (lowRes) pb.color(target, VK_ATTACHMENT_LOAD_OP_CLEAR, rhi::ClearColor::rgba(0, 0, 0, 0));
        else pb.color(target, VK_ATTACHMENT_LOAD_OP_LOAD);
        pb.read(depthForSoft, rhi::Access::SampledFragment);
        const rhi::RGTexture fog = translucency::declareLightingReads(pb, R);
        // Same graph as the simulation (first view): reuse its imports so the graph orders and syncs the passes.
        const bool reuse = frameState(ctx).firstViewOfFrame;
        for (const Prepared& p : list) {
            Job j{p, {}, {}, {}};
            auto it = m_imports.find(p.entityId);
            if (reuse && it != m_imports.end()) {
                j.particles = it->second[0], j.lists = it->second[1], j.counters = it->second[2];
            } else {
                j.particles = importBuffer(ctx, p.state->particles);
                j.lists = importBuffer(ctx, p.state->lists);
                j.counters = importBuffer(ctx, p.state->counters);
            }
            // The simulation flipped `current`: the list written this frame is the one now marked current.
            const u32 cap = p.gpu.maxParticles;
            if (p.state->lastSorted) {
                j.listOffset = cap * 3;
            } else {
                j.listOffset = cap * (1 + p.state->current);
            }
            pb.read(j.particles, rhi::Access::StorageReadGraphics)
                .read(j.lists, rhi::Access::StorageReadGraphics)
                .read(j.counters, rhi::Access::IndirectBuffer);
            jobs.push_back(j);
        }
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        FrameState* fs = &frameState(ctx);
        const glm::vec2 targetSize = lowRes ? glm::vec2(low.width, low.height) : glm::vec2(full.width, full.height);
        GpuScene* gs = &ctx.scene();
        pb.execute([=, this](rhi::PassContext& pcx) {
            rhi::CommandList& cmd = pcx.cmd;
            if (lowRes) {
                // Particles project with the full-resolution matrices: map the viewport onto the low-res target.
                cmd.setViewport(0.0f, 0.0f, f32(full.width) / f32(divisor), f32(full.height) / f32(divisor));
            }
            for (const Job& j : jobs) {
                RenderPush pc;
                pc.view = view, pc.scene = scene, pc.emitter = j.p.emitterAddress;
                pc.particles = pcx.address(j.particles);
                pc.lists = pcx.address(j.lists);
                pc.listOffset = j.listOffset, pc.listStride = j.listStride, pc.listSlot = j.listSlot;
                pc.depth = pcx.sampledIndex(depthForSoft);
                pc.fog = fog.valid() ? pcx.sampledIndex(fog) : kInvalidIndex;
                pc.lowRes = lowRes ? 1u : 0u;
                pc.targetSize = targetSize;
                const rhi::BufferHandle counters = pcx.buffer(j.counters);
                cmd.bindPipeline(m_draw[j.p.mesh ? 1 : 0]);
                cmd.pushConstants(pc);
                if (j.p.mesh) {
                    cmd.bindIndexBuffer(gs->indexBuffer());
                    cmd.drawIndexedIndirect(counters, kDrawIndexedOffset, 1);
                } else {
                    cmd.drawIndirect(counters, kDrawOffset, 1);
                }
                fs->stats->drawCalls += 1;
            }
        });
        if (lowRes) {
            g.addPass("Particles.Composite")
                .read(target, rhi::Access::SampledFragment)
                .read(depthForSoft, rhi::Access::SampledFragment)
                .read(src.depth, rhi::Access::SampledFragment)
                .color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD)
                .execute([=, this](rhi::PassContext& p) {
                    struct {
                        u64 view;
                        u32 particles, lowDepth, fullDepth, pad;
                        glm::vec2 lowSize;
                    } pc{view, p.sampledIndex(target), p.sampledIndex(depthForSoft), p.sampledIndex(src.depth), 0,
                         glm::vec2(f32(full.width) / f32(divisor), f32(full.height) / f32(divisor))};
                    drawFullscreen(p.cmd, m_composite, &pc, sizeof(pc));
                });
        }
    }

    rhi::PipelineHandle m_sim, m_sort, m_lowDepth, m_source, m_composite;
    rhi::PipelineHandle m_draw[2];
    rhi::TextureHandle m_sprites[3];
    std::unordered_map<u32, EmitterState> m_states;
    std::unordered_map<u32, std::array<rhi::RGBuffer, 3>> m_imports; // simulation imports of the current first view
    u64 m_frame = 0;
};

} // namespace

std::unique_ptr<IRenderFeature> makeParticlesFeature() { return std::make_unique<ParticlesFeature>(); }

} // namespace ox::render
