// Split rendering: portable segments on a secondary GPU (see split_rendering.hpp, docs/guide/35-split-rendering.md).
#include <oxwald/render/split_rendering.hpp>

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/render/renderer.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/format.hpp>
#include <oxwald/rhi/multi_device.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <memory>

namespace ox::render {

namespace {

CVar<int> cvSplit("r.SplitRendering", 0,
                  "Split rendering: run portable passes (post-process tail, independent compute) on a secondary GPU "
                  "(integrated). On = offload every portable pass, Auto = only when it saves primary GPU time",
                  CVarEnum{"Off", "On", "Auto"}, CVarFlags::Persist);
CVar<int> cvAdapter("r.SplitRendering.Adapter", -1,
                    "Secondary adapter index (Vulkan enumeration order); -1 = best GPU other than the primary (integrated first)",
                    -1, 16, CVarFlags::Persist);
CVar<bool> cvAllowSame("r.SplitRendering.AllowSameAdapter", false,
                       "Allow the secondary device on the primary GPU (tests the mechanism on single-GPU machines; no speed-up)");
CVar<int> cvTransfer("r.SplitRendering.Transfer", 0, "Cross-device transfer path",
                     CVarEnum{"Auto", "ExternalMemory", "HostPointer", "Staging"}, CVarFlags::Persist);
CVar<int> cvLatency("r.SplitRendering.Latency", 2,
                    "Frames between a segment's export and the use of its secondary results (1 = lowest latency, the CPU "
                    "then waits for the previous GPU frame)",
                    1, 3, CVarFlags::Persist);
CVar<float> cvMinGain("r.SplitRendering.MinGainMs", 0.1f,
                      "Auto: minimum primary GPU time (ms) a placement must save before anything is offloaded", 0.0f, 100.0f);
CVar<bool> cvCalibrate("r.SplitRendering.Calibrate", true,
                       "Measure the secondary/primary speed ratio with a post-process kernel when the secondary device is created");

constexpr u32 kEvaluateEveryFrames = 30;
constexpr u64 kForgetSegmentAfterFrames = 120;
constexpr u32 kImportTimeoutMs = 2000;

bool envAllowSameAdapter() {
    const char* v = std::getenv("OX_SPLIT_RENDERING_ALLOW_SAME_ADAPTER");
    return v && v[0] == '1';
}

struct Config {
    i32 adapter = -1;
    bool allowSame = false;
    i32 transfer = 0;
    bool calibrate = true;
    bool operator==(const Config&) const = default;
};

struct SegmentState {
    const RenderView* view = nullptr;
    std::string name;
    std::string viewName;
    std::vector<std::string> passNames;
    f32 costHint = 0.1f;

    // Channel (recreated when the texture signature or the latency changes).
    std::unique_ptr<rhi::CrossDeviceChannel> channel;
    std::vector<rhi::TextureDesc> descs;
    std::vector<u32> inputs, outputs;
    u32 latency = 0;
    bool portable = true;
    std::string unportableReason;

    struct Slot {
        u64 frame = ~0ull;
        bool exported = false;
        bool submitted = false;
        std::vector<rhi::PortableDispatch> dispatches;
    };
    std::vector<Slot> slots;

    bool offload = false;         // applied placement
    bool declaredOffload = false; // placement used by the last declaration
    u64 upBytes = 0, downBytes = 0;
    rhi::TimingFilter primaryMs{0.2f}, secondaryComputeMs{0.2f}, secondaryTotalMs{0.2f}, primaryTransferMs{0.2f},
        cpuCopyMs{0.2f};
    u64 lastSeenFrame = 0;
};

// Textures of a segment in order of first use, with their role.
struct SegmentTextures {
    std::vector<rhi::RGTexture> textures;
    std::vector<u32> inputs;  // read before written
    std::vector<u32> outputs; // last access is a write
    std::string error;
};

SegmentTextures analyze(const PortableSegmentDesc& desc) {
    SegmentTextures s;
    std::vector<bool> written, lastWrite;
    for (const PortablePassDesc& p : desc.passes) {
        for (const PortableTextureBinding& b : p.textures) {
            if (!b.texture.valid()) {
                s.error = std::format("'{}': invalid texture binding", p.name);
                return s;
            }
            auto it = std::find(s.textures.begin(), s.textures.end(), b.texture);
            u32 i = u32(it - s.textures.begin());
            if (it == s.textures.end()) {
                s.textures.push_back(b.texture);
                written.push_back(false);
                lastWrite.push_back(false);
                if (!b.write) s.inputs.push_back(i);
            }
            if (b.write) written[i] = true;
            lastWrite[i] = b.write;
        }
    }
    for (u32 i = 0; i < s.textures.size(); ++i) {
        if (lastWrite[i]) s.outputs.push_back(i);
    }
    return s;
}

void declarePrimary(FeatureContext& ctx, const PortableSegmentDesc& desc) {
    for (const PortablePassDesc& p : desc.passes) {
        rhi::PassBuilder pb = ctx.graph().addPass(p.name, rhi::PassType::Compute);
        std::vector<rhi::RGTexture> declared;
        for (const PortableTextureBinding& b : p.textures) {
            if (std::find(declared.begin(), declared.end(), b.texture) != declared.end()) continue;
            declared.push_back(b.texture);
            if (b.write) pb.overwrite(b.texture, rhi::Access::StorageWriteCompute);
            else pb.read(b.texture, rhi::Access::SampledCompute);
        }
        pb.execute([p](rhi::PassContext& c) {
            std::array<u8, rhi::kMaxPushConstantSize> push = p.push;
            for (const PortableTextureBinding& b : p.textures) {
                const u32 index = b.write ? c.storageIndex(b.texture) : c.sampledIndex(b.texture);
                std::memcpy(push.data() + b.pushOffset, &index, 4);
            }
            c.cmd.bindPipeline(p.pipeline);
            if (p.pushSize) c.cmd.pushConstants(push.data(), p.pushSize);
            c.cmd.dispatchThreads(p.threads[0], p.threads[1], p.threads[2]);
        });
    }
}

class SplitRenderingFeature;

} // namespace

// The per-renderer state lives in a feature so it gets the renderer's lifetime (initialize/shutdown) and a per-frame
// hook (PreDepth, before any other feature declares passes).
namespace {

class SplitRenderingFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "SplitRendering"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::PreDepth); }
    i32 order() const override { return -1000000; }
    std::vector<std::string> cvarNames() const override {
        return {"r.SplitRendering", "r.SplitRendering.Adapter", "r.SplitRendering.AllowSameAdapter", "r.SplitRendering.Transfer",
                "r.SplitRendering.Latency", "r.SplitRendering.MinGainMs", "r.SplitRendering.Calibrate"};
    }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return true; }
    void shutdown(rhi::Device&) override { teardown("renderer shut down"); }

    void setup(FeatureContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const u64 frame = dev.frameNumber();
        if (frame != m_lastTick) {
            m_lastTick = frame;
            frameTick(ctx);
        }
        publish(ctx.stats().split);
    }

    [[nodiscard]] bool tickedThisFrame(const rhi::Device& dev) const { return m_lastTick == dev.frameNumber(); }

    void declare(FeatureContext& ctx, const PortableSegmentDesc& desc);

    SplitRenderingStats status(Renderer& r) const {
        SplitRenderingStats s = r.stats().split;
        publish(s);
        return s;
    }

    void simulateDeviceLost() {
        if (m_secondary) m_secondary->simulateDeviceLost();
    }

private:
    void frameTick(FeatureContext& ctx);
    void create(rhi::Device& dev);
    void teardown(std::string_view why);
    void evaluate(FeatureContext& ctx);
    void collectPrimaryTimings(const std::vector<rhi::GpuTiming>& timings);
    SegmentState& segment(const RenderView* view, const std::string& name);
    bool ensureChannel(SegmentState& seg, const std::vector<rhi::TextureDesc>& descs, const SegmentTextures& tex, u32 latency);

    void publish(SplitRenderingStats& s) const {
        s.mode = m_mode;
        s.active = m_secondary != nullptr;
        s.sameAdapter = m_secondary && m_secondary->sameAdapter();
        s.adapter = m_secondary ? m_secondary->adapter().name : std::string();
        s.transfer = m_secondary ? rhi::crossDeviceTransferName(m_secondary->transferPath()) : "";
        s.latencyFrames = m_secondary ? u32(cvLatency.get()) : 0;
        s.secondaryGpuMs = m_secondaryGpuMs;
        s.secondaryComputeMs = m_secondaryComputeMs;
        s.transferMs = m_transferMs;
        s.cpuCopyMs = m_cpuCopyMs;
        s.predictedSavingMs = m_predictedSavingMs;
        s.speedRatio = speedRatio();
        s.status = m_status;
    }

    [[nodiscard]] f32 speedRatio() const {
        if (m_measuredRatio.valid()) return m_measuredRatio.value();
        if (m_secondary && m_secondary->calibratedSpeedRatio() > 0.0f) return m_secondary->calibratedSpeedRatio();
        return 0.0f;
    }

    i32 m_mode = 0;
    Config m_config;
    bool m_haveConfig = false;
    bool m_usableThisFrame = false;
    std::string m_blocked; // creation failed / device lost: no retry until the mode or config changes
    std::unique_ptr<rhi::SecondaryDevice> m_secondary;
    std::vector<std::unique_ptr<SegmentState>> m_segments;
    rhi::SplitController m_controller{3};
    u64 m_lastTick = ~0ull;
    u32 m_framesSinceEval = kEvaluateEveryFrames;
    rhi::TimingFilter m_measuredRatio{0.1f};
    f64 m_secondaryGpuMs = 0.0, m_secondaryComputeMs = 0.0, m_transferMs = 0.0, m_cpuCopyMs = 0.0;
    f64 m_predictedSavingMs = 0.0;
    std::string m_status = "off";
};

void SplitRenderingFeature::teardown(std::string_view why) {
    if (m_segments.empty() && !m_secondary) return;
    OX_PROFILE_ZONE();
    for (auto& s : m_segments) s->channel.reset(); // waits for the segment's jobs
    m_segments.clear();
    if (m_secondary) OX_LOG_INFO("render", "split rendering: secondary device released ({})", why);
    m_secondary.reset();
    m_controller.reset();
    m_measuredRatio.reset();
    m_secondaryGpuMs = m_secondaryComputeMs = m_transferMs = m_cpuCopyMs = m_predictedSavingMs = 0.0;
}

void SplitRenderingFeature::create(rhi::Device& dev) {
    OX_PROFILE_ZONE();
    rhi::SecondaryDeviceDesc sd;
    sd.adapterIndex = m_config.adapter;
    sd.allowSameAdapter = m_config.allowSame;
    sd.transfer = rhi::CrossDeviceTransfer(std::clamp(m_config.transfer, 0, 3));
    sd.calibrate = m_config.calibrate;
    std::string error;
    m_secondary = rhi::SecondaryDevice::create(dev, sd, &error);
    if (!m_secondary) {
        m_blocked = "no secondary device: " + error;
        OX_LOG_INFO("render", "split rendering inactive: {}", m_blocked);
        return;
    }
    OX_LOG_INFO("render", "split rendering: secondary '{}' ({}), transfer {}, calibrated speed ratio {:.2f}",
                m_secondary->adapter().name, rhi::adapterTypeName(m_secondary->adapter().type),
                rhi::crossDeviceTransferName(m_secondary->transferPath()), m_secondary->calibratedSpeedRatio());
}

void SplitRenderingFeature::frameTick(FeatureContext& ctx) {
    OX_PROFILE_ZONE();
    rhi::Device& dev = ctx.device();
    m_mode = std::clamp(cvSplit.get(), 0, 2);
    m_usableThisFrame = false;
    if (m_mode == i32(SplitRenderingMode::Off)) {
        teardown("r.SplitRendering 0");
        m_blocked.clear();
        m_status = "off";
        return;
    }
    const Config cfg{cvAdapter.get(), cvAllowSame.get() || envAllowSameAdapter(), cvTransfer.get(), cvCalibrate.get()};
    if (!m_haveConfig || !(cfg == m_config)) {
        teardown("configuration changed");
        m_config = cfg;
        m_haveConfig = true;
        m_blocked.clear();
    }
    if (m_secondary && m_secondary->lost()) {
        const std::string reason = m_secondary->lostReason();
        teardown("device lost");
        m_blocked = std::format("secondary device lost ({}): split rendering disabled until r.SplitRendering is toggled", reason);
        OX_LOG_WARN("render", "{}", m_blocked);
    }
    if (!m_secondary && m_blocked.empty()) create(dev);
    m_usableThisFrame = m_secondary != nullptr;
    if (!m_secondary) {
        m_status = m_blocked;
        return;
    }

    // Jobs exported by earlier frames: their primary submission is now known (conservatively the last one).
    const rhi::TimelinePoint exported = dev.lastSubmitted(rhi::QueueType::Graphics);
    for (auto& seg : m_segments) {
        for (u32 i = 0; i < seg->slots.size(); ++i) {
            SegmentState::Slot& s = seg->slots[i];
            if (!s.exported || s.submitted || !seg->channel) continue;
            seg->channel->submit(i, s.frame, exported, std::move(s.dispatches));
            s.dispatches.clear();
            s.submitted = true;
        }
    }
    collectPrimaryTimings(dev.gpuTimings());

    // Forget segments that were not declared for a while (view closed, feature disabled).
    const u64 frame = dev.frameNumber();
    const usize before = m_segments.size();
    std::erase_if(m_segments, [&](const std::unique_ptr<SegmentState>& s) { return frame > s->lastSeenFrame + kForgetSegmentAfterFrames; });
    if (m_segments.size() != before) m_controller.reset();

    m_secondaryGpuMs = m_secondaryComputeMs = m_transferMs = m_cpuCopyMs = 0.0;
    for (const auto& s : m_segments) {
        if (!s->declaredOffload) continue;
        m_secondaryGpuMs += s->secondaryTotalMs.value();
        m_secondaryComputeMs += s->secondaryComputeMs.value();
        m_transferMs += s->primaryTransferMs.value() + (s->secondaryTotalMs.value() - s->secondaryComputeMs.value()) +
                        s->cpuCopyMs.value();
        m_cpuCopyMs += s->cpuCopyMs.value();
    }
    evaluate(ctx);
}

void SplitRenderingFeature::collectPrimaryTimings(const std::vector<rhi::GpuTiming>& timings) {
    auto sum = [&](const std::string& view, const std::string& pass, bool& found) {
        f64 ms = 0.0;
        const std::string full = view + "/" + pass;
        for (const rhi::GpuTiming& t : timings) {
            if (t.name == full || t.name == pass) {
                ms += t.milliseconds;
                found = true;
            }
        }
        return ms;
    };
    for (auto& seg : m_segments) {
        if (seg->lastSeenFrame + 4 < m_lastTick) continue;
        bool found = false;
        f64 ms = 0.0;
        for (const std::string& p : seg->passNames) ms += sum(seg->viewName, p, found);
        if (found && !seg->declaredOffload) seg->primaryMs.add(f32(ms));
        bool transfer = false;
        const f64 tms = sum(seg->viewName, "Split.Export." + seg->name, transfer) + sum(seg->viewName, "Split.Import." + seg->name, transfer);
        if (transfer && seg->declaredOffload) seg->primaryTransferMs.add(f32(tms));
        if (seg->primaryMs.valid() && seg->secondaryComputeMs.valid() && seg->primaryMs.value() > 0.005f) {
            m_measuredRatio.add(seg->secondaryComputeMs.value() / seg->primaryMs.value());
        }
    }
}

void SplitRenderingFeature::evaluate(FeatureContext& ctx) {
    if (m_segments.empty()) {
        m_status = "active, no portable passes declared";
        return;
    }
    if (m_mode == i32(SplitRenderingMode::On)) {
        m_predictedSavingMs = 0.0;
        for (auto& s : m_segments) {
            s->offload = s->portable;
            m_predictedSavingMs += s->offload ? s->primaryMs.value(s->costHint) - s->primaryTransferMs.value() : 0.0;
        }
        m_status = "On: every portable segment offloaded";
        return;
    }
    if (++m_framesSinceEval < kEvaluateEveryFrames) return;
    m_framesSinceEval = 0;
    const rhi::CrossDeviceTransfer path = m_secondary->transferPath();
    rhi::SplitCostModel model;
    model.bandwidth = rhi::defaultTransferBandwidth(path, m_secondary->sameAdapter());
    model.minGainMs = cvMinGain.get();
    const f64 frameMs = ctx.renderer().stats().gpuFrameMs;
    model.primaryFrameMs = frameMs > 0.0 ? f32(frameMs) : 16.6f;
    // Measured primary copy bandwidth replaces the default once offloaded segments were timed.
    f64 bytes = 0.0, ms = 0.0;
    for (const auto& s : m_segments) {
        if (s->primaryTransferMs.valid()) {
            bytes += f64(s->upBytes + s->downBytes);
            ms += std::max(s->primaryTransferMs.value() - 2.0f * model.bandwidth.fixedMs, 0.001f);
        }
    }
    if (ms > 0.0) model.bandwidth.primaryCopyGBps = f32(bytes / (ms * 1.0e6));
    const f32 ratio = speedRatio() > 0.0f ? speedRatio() : 10.0f; // unknown: assume a much slower iGPU
    std::vector<rhi::SplitSegmentCost> costs;
    for (const auto& s : m_segments) {
        rhi::SplitSegmentCost c;
        c.name = s->name;
        c.portable = s->portable;
        c.primaryMs = s->primaryMs.value(s->costHint);
        c.secondaryMs = s->secondaryComputeMs.valid() ? s->secondaryComputeMs.value() : c.primaryMs * ratio;
        c.uploadBytes = s->upBytes;
        c.downloadBytes = s->downBytes;
        costs.push_back(c);
    }
    const rhi::SplitDecision d = rhi::chooseSplit(costs, model);
    const std::vector<bool>& placement = m_controller.update(d);
    m_predictedSavingMs = 0.0;
    for (usize i = 0; i < m_segments.size(); ++i) {
        m_segments[i]->offload = i < placement.size() && placement[i];
        if (m_segments[i]->offload) m_predictedSavingMs += d.segments[i].primarySavedMs;
    }
    m_status = "Auto: " + d.reason;
}

SegmentState& SplitRenderingFeature::segment(const RenderView* view, const std::string& name) {
    for (auto& s : m_segments) {
        if (s->view == view && s->name == name) return *s;
    }
    auto s = std::make_unique<SegmentState>();
    s->view = view;
    s->name = name;
    s->offload = m_mode == i32(SplitRenderingMode::On);
    m_segments.push_back(std::move(s));
    m_controller.reset();
    m_framesSinceEval = kEvaluateEveryFrames; // evaluate the new set soon
    return *m_segments.back();
}

bool SplitRenderingFeature::ensureChannel(SegmentState& seg, const std::vector<rhi::TextureDesc>& descs,
                                          const SegmentTextures& tex, u32 latency) {
    if (seg.channel && seg.descs == descs && seg.inputs == tex.inputs && seg.outputs == tex.outputs && seg.latency == latency) {
        return true;
    }
    seg.channel.reset(); // waits for its jobs
    seg.slots.clear();
    seg.descs = descs;
    seg.inputs = tex.inputs;
    seg.outputs = tex.outputs;
    seg.latency = latency;
    rhi::CrossDeviceChannelDesc cd;
    cd.name = seg.name;
    cd.textures = descs;
    cd.inputs = tex.inputs;
    cd.outputs = tex.outputs;
    cd.slots = latency + 1;
    std::string error;
    seg.channel = m_secondary->createChannel(cd, &error);
    if (!seg.channel) {
        seg.portable = false;
        seg.unportableReason = error;
        OX_LOG_WARN("render", "split rendering: segment '{}' stays on the primary: {}", seg.name, error);
        return false;
    }
    seg.slots.assign(cd.slots, {});
    seg.upBytes = seg.channel->uploadBytes();
    seg.downBytes = seg.channel->downloadBytes();
    return true;
}

void SplitRenderingFeature::declare(FeatureContext& ctx, const PortableSegmentDesc& desc) {
    rhi::Device& dev = ctx.device();
    rhi::RenderGraph& graph = ctx.graph();
    RenderStats& stats = ctx.stats();
    const u64 frame = dev.frameNumber();
    SegmentState& seg = segment(&ctx.view(), desc.name);
    seg.lastSeenFrame = frame;
    seg.costHint = desc.costHintMs;
    seg.viewName = ctx.view().desc().name;
    seg.passNames.clear();
    for (const PortablePassDesc& p : desc.passes) seg.passNames.push_back(p.name);
    stats.split.segments += 1;

    const SegmentTextures tex = analyze(desc);
    // Placement is decided once per frame at the tick: a loss discovered later in the frame goes through the import
    // fallback instead of changing the graph half-way.
    bool offload = m_secondary && m_usableThisFrame && seg.offload && seg.portable && desc.portable && tex.error.empty() &&
                   !tex.outputs.empty();
    for (const PortablePassDesc& p : desc.passes) offload = offload && dev.vkPipeline(p.pipeline) != VK_NULL_HANDLE;
    std::vector<rhi::TextureDesc> descs;
    if (offload) {
        for (rhi::RGTexture t : tex.textures) {
            rhi::TextureDesc d = graph.textureDesc(t);
            d.usage = rhi::TextureUsage::None;
            descs.push_back(d);
        }
        offload = ensureChannel(seg, descs, tex, u32(std::clamp(cvLatency.get(), 1, 3)));
    }
    seg.declaredOffload = offload;
    if (!offload) {
        declarePrimary(ctx, desc);
        return;
    }
    stats.split.offloaded += 1;

    const u32 latency = seg.latency;
    const u32 slotCount = latency + 1;
    rhi::CrossDeviceChannel* channel = seg.channel.get();

    // --- export this frame's inputs (the worker runs the passes once this frame retired on the primary) ---
    const u32 slot = u32(frame % slotCount);
    SegmentState::Slot& cur = seg.slots[slot];
    if (cur.submitted && cur.frame != ~0ull && !channel->ready(slot, cur.frame)) {
        // An unconsumed job still owns the slot (placement or latency changed): wait before overwriting it.
        const auto t0 = std::chrono::steady_clock::now();
        channel->wait(slot, cur.frame, kImportTimeoutMs);
        stats.split.waitMs += std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    cur = {};
    cur.frame = frame;
    cur.exported = true;
    for (const PortablePassDesc& p : desc.passes) {
        rhi::PortableDispatch d;
        d.name = p.name;
        d.pipeline = p.pipeline;
        d.push = p.push;
        d.pushSize = p.pushSize;
        d.threads = p.threads;
        for (const PortableTextureBinding& b : p.textures) {
            const u32 index = u32(std::find(tex.textures.begin(), tex.textures.end(), b.texture) - tex.textures.begin());
            d.bindings.push_back({b.pushOffset, index, b.write});
        }
        cur.dispatches.push_back(std::move(d));
    }
    std::vector<rhi::RGTexture> inputs, outputs;
    for (u32 i : tex.inputs) inputs.push_back(tex.textures[i]);
    for (u32 i : tex.outputs) outputs.push_back(tex.textures[i]);
    if (!inputs.empty()) {
        rhi::PassBuilder pb = graph.addPass("Split.Export." + desc.name, rhi::PassType::Transfer);
        for (rhi::RGTexture t : inputs) pb.read(t, rhi::Access::TransferRead);
        pb.sideEffect().execute([channel, slot, inputs](rhi::PassContext& p) {
            std::vector<rhi::TextureHandle> handles;
            for (rhi::RGTexture t : inputs) handles.push_back(p.texture(t));
            channel->recordExport(p.cmd, slot, handles);
        });
    }

    // --- import the results of frame - latency (warm-up: run on the primary) ---
    const bool haveResult = frame >= latency && seg.slots[(frame - latency) % slotCount].frame == frame - latency &&
                            seg.slots[(frame - latency) % slotCount].submitted;
    if (!haveResult) {
        declarePrimary(ctx, desc);
        return;
    }
    const u64 resultFrame = frame - latency;
    const u32 resultSlot = u32(resultFrame % slotCount);
    // Fallback source per output (same format and size): copied through if the secondary job failed.
    std::vector<rhi::RGTexture> fallback(outputs.size());
    rhi::PassBuilder pb = graph.addPass("Split.Import." + desc.name, rhi::PassType::Transfer);
    std::vector<rhi::RGTexture> readDeclared;
    for (usize o = 0; o < outputs.size(); ++o) {
        const bool alsoInput = std::find(inputs.begin(), inputs.end(), outputs[o]) != inputs.end();
        if (alsoInput) {
            pb.write(outputs[o], rhi::Access::TransferWrite);
            continue;
        }
        pb.overwrite(outputs[o], rhi::Access::TransferWrite);
        const rhi::TextureDesc& od = graph.textureDesc(outputs[o]);
        for (rhi::RGTexture in : inputs) {
            const rhi::TextureDesc& id = graph.textureDesc(in);
            const bool inputIsOutput = std::find(outputs.begin(), outputs.end(), in) != outputs.end();
            if (!inputIsOutput && id.format == od.format && id.width == od.width && id.height == od.height) {
                fallback[o] = in;
                if (std::find(readDeclared.begin(), readDeclared.end(), in) == readDeclared.end()) {
                    pb.read(in, rhi::Access::TransferRead);
                    readDeclared.push_back(in);
                }
                break;
            }
        }
    }
    SegmentState* segPtr = &seg;
    RenderStats* statsPtr = &stats;
    pb.execute([channel, resultSlot, resultFrame, outputs, fallback, segPtr, statsPtr](rhi::PassContext& p) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = channel->wait(resultSlot, resultFrame, kImportTimeoutMs);
        statsPtr->split.waitMs += std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ok) {
            std::vector<rhi::TextureHandle> handles;
            for (rhi::RGTexture t : outputs) handles.push_back(p.texture(t));
            channel->recordImport(p.cmd, resultSlot, handles);
            const rhi::CrossDeviceJobTiming t = channel->timing(resultSlot);
            segPtr->secondaryComputeMs.add(f32(t.computeMs));
            segPtr->secondaryTotalMs.add(f32(t.uploadMs + t.computeMs + t.downloadMs));
            segPtr->cpuCopyMs.add(f32(t.cpuCopyMs));
            statsPtr->split.imported += 1;
            return;
        }
        // Secondary failed (device lost / timeout): keep the frame valid, the next frame tick disables the split.
        statsPtr->split.fallbacks += 1;
        for (usize o = 0; o < outputs.size(); ++o) {
            const rhi::TextureHandle dst = p.texture(outputs[o]);
            if (fallback[o].valid()) {
                p.cmd.copyTexture(p.texture(fallback[o]), dst);
            } else {
                const VkClearColorValue black{};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(p.cmd.vk(), p.device.vkImage(dst), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            }
        }
    });
}

SplitRenderingFeature* findFeature(Renderer& r) {
    return dynamic_cast<SplitRenderingFeature*>(r.features().find("SplitRendering"));
}

} // namespace

void addPortableSegment(FeatureContext& ctx, const PortableSegmentDesc& desc) {
    SplitRenderingFeature* f = findFeature(ctx.renderer());
    if (!f || !f->tickedThisFrame(ctx.device())) {
        declarePrimary(ctx, desc);
        return;
    }
    f->declare(ctx, desc);
}

void appendSplitRenderingVulkanExtensions(rhi::DeviceDesc& desc) { rhi::appendMultiDeviceExtensions(desc); }

SplitRenderingStats splitRenderingStatus(Renderer& renderer) {
    if (SplitRenderingFeature* f = findFeature(renderer)) return f->status(renderer);
    return renderer.stats().split;
}

void splitRenderingSimulateDeviceLost(Renderer& renderer) {
    if (SplitRenderingFeature* f = findFeature(renderer)) f->simulateDeviceLost();
}

void registerSplitRenderingFeature(FeatureRegistry& registry) { registry.add(std::make_unique<SplitRenderingFeature>()); }

} // namespace ox::render
