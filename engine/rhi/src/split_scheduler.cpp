#include <oxwald/rhi/split_scheduler.hpp>

#include <algorithm>
#include <format>

namespace ox::rhi {

const char* crossDeviceTransferName(CrossDeviceTransfer t) {
    switch (t) {
    case CrossDeviceTransfer::Auto: return "Auto";
    case CrossDeviceTransfer::ExternalMemory: return "ExternalMemory";
    case CrossDeviceTransfer::HostPointer: return "HostPointer";
    case CrossDeviceTransfer::Staging: return "Staging";
    }
    return "?";
}

bool transferPathAvailable(const CrossDeviceCaps& caps, CrossDeviceTransfer path) {
    switch (path) {
    case CrossDeviceTransfer::Auto: return true;
    case CrossDeviceTransfer::ExternalMemory:
        return caps.sameDevice && caps.primaryExportOpaque && caps.secondaryImportOpaque;
    case CrossDeviceTransfer::HostPointer: return caps.primaryHostPointer && caps.secondaryHostPointer;
    case CrossDeviceTransfer::Staging: return true;
    }
    return false;
}

static std::string whyUnavailable(const CrossDeviceCaps& caps, CrossDeviceTransfer path) {
    if (path == CrossDeviceTransfer::ExternalMemory) {
        if (!caps.sameDevice) return "opaque memory handles are only valid between devices with the same deviceUUID/driverUUID";
        if (!caps.primaryExportOpaque) return "the primary device cannot export opaque memory (VK_KHR_external_memory_win32/fd not enabled)";
        return "the secondary device cannot import opaque memory";
    }
    if (path == CrossDeviceTransfer::HostPointer) {
        if (!caps.primaryHostPointer) return "VK_EXT_external_memory_host not enabled on the primary device";
        return "VK_EXT_external_memory_host not available on the secondary device";
    }
    return {};
}

TransferPathChoice selectTransferPath(const CrossDeviceCaps& caps, CrossDeviceTransfer preference) {
    TransferPathChoice out;
    std::string prefix;
    if (preference != CrossDeviceTransfer::Auto) {
        if (transferPathAvailable(caps, preference)) {
            out.path = preference;
            out.reason = std::format("{} requested", crossDeviceTransferName(preference));
            return out;
        }
        prefix = std::format("{} requested but unavailable ({}); ", crossDeviceTransferName(preference),
                             whyUnavailable(caps, preference));
    }
    if (transferPathAvailable(caps, CrossDeviceTransfer::ExternalMemory)) {
        out.path = CrossDeviceTransfer::ExternalMemory;
        out.reason = prefix + "same physical device: one shared allocation";
    } else if (transferPathAvailable(caps, CrossDeviceTransfer::HostPointer)) {
        out.path = CrossDeviceTransfer::HostPointer;
        out.reason = prefix + "host memory imported into both devices (no CPU copies)";
    } else {
        out.path = CrossDeviceTransfer::Staging;
        out.reason = prefix + "no shareable memory: host-visible staging buffers + CPU copies";
    }
    return out;
}

TransferBandwidth defaultTransferBandwidth(CrossDeviceTransfer path, bool sameDevice) {
    TransferBandwidth b;
    switch (path) {
    case CrossDeviceTransfer::ExternalMemory:
        // Both copies stay in the local memory of one GPU.
        b.primaryCopyGBps = 200.0f;
        b.secondaryCopyGBps = sameDevice ? 200.0f : 15.0f;
        b.cpuCopyGBps = 0.0f;
        b.fixedMs = 0.03f;
        break;
    case CrossDeviceTransfer::HostPointer:
        // Discrete GPU writes/reads system memory over PCIe; an integrated GPU's memory *is* system memory.
        b.primaryCopyGBps = 11.0f;
        b.secondaryCopyGBps = sameDevice ? 11.0f : 15.0f;
        b.cpuCopyGBps = 0.0f;
        b.fixedMs = 0.05f;
        break;
    case CrossDeviceTransfer::Staging:
    case CrossDeviceTransfer::Auto:
        b.primaryCopyGBps = 11.0f;
        b.secondaryCopyGBps = sameDevice ? 11.0f : 15.0f;
        b.cpuCopyGBps = 6.0f; // two memcpys through uncached/write-combined memory
        b.fixedMs = 0.05f;
        break;
    }
    return b;
}

static f32 bytesToMs(u64 bytes, f32 gbps) { return gbps > 0.0f ? f32(f64(bytes) / (f64(gbps) * 1.0e6)) : 0.0f; }

SplitSegmentPlan estimateSegment(const SplitSegmentCost& segment, const SplitCostModel& model) {
    SplitSegmentPlan p;
    const u64 bytes = segment.uploadBytes + segment.downloadBytes;
    const TransferBandwidth& b = model.bandwidth;
    p.primaryTransferMs = bytesToMs(bytes, b.primaryCopyGBps) + 2.0f * b.fixedMs;
    p.secondaryLoadMs = segment.secondaryMs + bytesToMs(bytes, b.secondaryCopyGBps) + b.fixedMs;
    p.cpuCopyMs = bytesToMs(bytes, b.cpuCopyGBps);
    p.primarySavedMs = segment.primaryMs - p.primaryTransferMs;
    return p;
}

SplitDecision chooseSplit(std::span<const SplitSegmentCost> segments, const SplitCostModel& model, bool force) {
    SplitDecision d;
    d.segments.resize(segments.size());
    std::vector<u32> candidates;
    for (u32 i = 0; i < segments.size(); ++i) {
        d.segments[i] = estimateSegment(segments[i], model);
        if (segments[i].portable) candidates.push_back(i);
    }
    auto finish = [&](std::string reason) {
        d.primarySavedMs = 0.0f;
        d.secondaryLoadMs = 0.0f;
        for (const SplitSegmentPlan& p : d.segments) {
            if (!p.offload) continue;
            d.primarySavedMs += p.primarySavedMs;
            d.secondaryLoadMs += p.secondaryLoadMs + p.cpuCopyMs;
            d.offloadsAnything = true;
        }
        d.newPrimaryFrameMs = model.primaryFrameMs - d.primarySavedMs;
        d.reason = std::move(reason);
        return d;
    };
    if (candidates.empty()) return finish("no portable segments");
    if (force) {
        for (u32 i : candidates) d.segments[i].offload = true;
        return finish("forced (r.SplitRendering 1)");
    }

    auto feasible = [&](f32 saved, f32 load) {
        const f32 frame = std::max(model.primaryFrameMs - saved, 0.0f);
        return load <= model.secondaryBudgetFraction * frame;
    };
    f32 bestSaved = 0.0f;
    std::vector<u32> best;
    const u32 n = u32(candidates.size());
    if (n <= 16) {
        for (u32 mask = 1; mask < (1u << n); ++mask) {
            f32 saved = 0.0f, load = 0.0f;
            for (u32 k = 0; k < n; ++k) {
                if (!(mask & (1u << k))) continue;
                const SplitSegmentPlan& p = d.segments[candidates[k]];
                saved += p.primarySavedMs;
                load += p.secondaryLoadMs + p.cpuCopyMs;
            }
            if (saved > bestSaved && feasible(saved, load)) {
                bestSaved = saved;
                best.clear();
                for (u32 k = 0; k < n; ++k) {
                    if (mask & (1u << k)) best.push_back(candidates[k]);
                }
            }
        }
    } else {
        std::vector<u32> order;
        for (u32 i : candidates) {
            if (d.segments[i].primarySavedMs > 0.0f) order.push_back(i);
        }
        auto ratio = [&](u32 i) {
            const SplitSegmentPlan& p = d.segments[i];
            return p.primarySavedMs / std::max(p.secondaryLoadMs + p.cpuCopyMs, 1e-4f);
        };
        std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return ratio(a) > ratio(b); });
        f32 saved = 0.0f, load = 0.0f;
        for (u32 i : order) {
            const SplitSegmentPlan& p = d.segments[i];
            if (feasible(saved + p.primarySavedMs, load + p.secondaryLoadMs + p.cpuCopyMs)) {
                saved += p.primarySavedMs;
                load += p.secondaryLoadMs + p.cpuCopyMs;
                best.push_back(i);
            }
        }
        bestSaved = saved;
    }
    if (best.empty() || bestSaved < model.minGainMs) {
        f32 top = -1e9f;
        for (u32 i : candidates) top = std::max(top, d.segments[i].primarySavedMs);
        return finish(best.empty() && top > 0.0f
                          ? std::format("secondary too slow: offloading would make it the bottleneck (best saving {:.3f} ms)", top)
                          : std::format("not profitable: best primary saving {:.3f} ms < {:.3f} ms", std::max(bestSaved, top),
                                        model.minGainMs));
    }
    for (u32 i : best) d.segments[i].offload = true;
    return finish(std::format("offloading {} of {} segments saves {:.3f} ms on the primary", best.size(), n, bestSaved));
}

void TimingFilter::add(f32 sample) {
    if (!(sample >= 0.0f)) return; // rejects NaN and negatives
    m_value = m_valid ? m_value + m_alpha * (sample - m_value) : sample;
    m_valid = true;
    m_samples += 1;
}

const std::vector<bool>& SplitController::update(const SplitDecision& decision) {
    std::vector<bool> target(decision.segments.size());
    for (usize i = 0; i < target.size(); ++i) target[i] = decision.segments[i].offload;
    if (m_applied.size() != target.size()) {
        m_applied.assign(target.size(), false);
        m_pending.clear();
        m_agree = 0;
    }
    if (target == m_applied) {
        m_pending.clear();
        m_agree = 0;
        return m_applied;
    }
    if (target == m_pending) {
        m_agree += 1;
    } else {
        m_pending = target;
        m_agree = 1;
    }
    if (m_agree >= std::max(m_confirmations, 1u)) {
        m_applied = target;
        m_pending.clear();
        m_agree = 0;
    }
    return m_applied;
}

void SplitController::reset() {
    m_applied.clear();
    m_pending.clear();
    m_agree = 0;
}

} // namespace ox::rhi
