#pragma once

// Split rendering (explicit multi-adapter) planning: pure CPU, no Vulkan objects, unit tested.
//
// A frame is a sequence of "segments": runs of portable compute passes that may execute either on the primary
// (discrete) device or on a secondary device (typically the integrated GPU). Offloading a segment costs two copies on
// the primary (inputs out, results back), copies on the secondary, optionally CPU memcpys (staging path), and one
// frame of latency; it saves the segment's primary GPU time. The secondary runs pipelined with the primary, so its
// total load per frame has to fit into the (new) primary frame time or it becomes the bottleneck.
// See docs/guide/35-split-rendering.md.

#include <oxwald/core/types.hpp>

#include <span>
#include <string>
#include <vector>

namespace ox::rhi {

// How data moves between the two devices.
enum class CrossDeviceTransfer : u8 {
    Auto,           // best available, in the order below
    ExternalMemory, // one allocation shared by both VkDevices (opaque Win32/fd handle): same physical device only
    HostPointer,    // VK_EXT_external_memory_host: host memory imported into both devices, GPU copies only
    Staging,        // each device has its own host-visible buffer, the CPU copies between them
};
const char* crossDeviceTransferName(CrossDeviceTransfer t);

// What the two devices can share (filled by SecondaryDevice from the drivers' external memory queries).
struct CrossDeviceCaps {
    bool sameDevice = false;            // deviceUUID and driverUUID match (opaque handles are only valid then)
    bool luidMatch = false;             // Windows adapter LUIDs match
    bool primaryExportOpaque = false;   // primary can export an opaque Win32/fd buffer allocation
    bool secondaryImportOpaque = false; // secondary can import it
    bool primaryHostPointer = false;    // VK_EXT_external_memory_host enabled on the primary
    bool secondaryHostPointer = false;  // ... and on the secondary
    u64 hostPointerAlignment = 0;       // max(minImportedHostPointerAlignment) of both devices
};

struct TransferPathChoice {
    CrossDeviceTransfer path = CrossDeviceTransfer::Staging;
    std::string reason; // why this path (and why a requested one was not available)
};

// `preference` = Auto picks ExternalMemory > HostPointer > Staging. An unavailable explicit preference falls back to
// the Auto choice and says so in `reason`. Staging is always possible.
TransferPathChoice selectTransferPath(const CrossDeviceCaps& caps, CrossDeviceTransfer preference);
bool transferPathAvailable(const CrossDeviceCaps& caps, CrossDeviceTransfer path);

// Copy bandwidths of one path (GB/s = bytes per ns). Measured values replace these defaults at run time.
struct TransferBandwidth {
    f32 primaryCopyGBps = 12.0f;   // primary copy image <-> shared/staging buffer (PCIe for host memory)
    f32 secondaryCopyGBps = 15.0f; // secondary copy shared/staging buffer <-> image
    f32 cpuCopyGBps = 0.0f;        // CPU memcpy between the devices' staging buffers (Staging only, 0 = none)
    f32 fixedMs = 0.05f;           // per transfer (submission, synchronisation, pass overhead)
};
// Conservative defaults: same physical device (VRAM-local copies), host memory over PCIe 3.0 x16, CPU memcpy.
TransferBandwidth defaultTransferBandwidth(CrossDeviceTransfer path, bool sameDevice);

struct SplitSegmentCost {
    std::string name;
    bool portable = true;   // false: must stay on the primary (non-portable pass, unsupported format, ...)
    f32 primaryMs = 0.0f;   // GPU time on the primary device (measured, else the declared hint)
    f32 secondaryMs = 0.0f; // on the secondary (measured, else primaryMs * speed ratio)
    u64 uploadBytes = 0;    // inputs, primary -> secondary
    u64 downloadBytes = 0;  // results, secondary -> primary
};

struct SplitCostModel {
    TransferBandwidth bandwidth;
    f32 primaryFrameMs = 16.6f;          // current primary GPU frame time (before offloading)
    f32 minGainMs = 0.1f;                // net primary saving required to offload anything
    f32 secondaryBudgetFraction = 0.9f;  // secondary (+ CPU copies) may be busy at most this fraction of a frame
};

struct SplitSegmentPlan {
    bool offload = false;
    f32 primarySavedMs = 0.0f;    // primaryMs - primaryTransferMs - fixed overhead (may be negative)
    f32 primaryTransferMs = 0.0f; // copies the primary records (export + import)
    f32 secondaryLoadMs = 0.0f;   // secondary compute + its copies
    f32 cpuCopyMs = 0.0f;         // worker-thread memcpys (Staging)
};

struct SplitDecision {
    std::vector<SplitSegmentPlan> segments; // parallel to the input
    f32 primarySavedMs = 0.0f;              // of the offloaded set
    f32 secondaryLoadMs = 0.0f;             // secondary + CPU copies of the offloaded set
    f32 newPrimaryFrameMs = 0.0f;
    bool offloadsAnything = false;
    std::string reason;
};

// Costs of offloading one segment under `model` (no decision).
SplitSegmentPlan estimateSegment(const SplitSegmentCost& segment, const SplitCostModel& model);

// Picks the set of portable segments that maximises the primary saving while the secondary keeps up with the new
// frame time (exact subset search up to 16 segments, greedy by saving per secondary ms beyond that). Nothing is
// offloaded when the best saving is below minGainMs. force = offload every portable segment (r.SplitRendering 1).
SplitDecision chooseSplit(std::span<const SplitSegmentCost> segments, const SplitCostModel& model, bool force = false);

// Exponential moving average of a GPU time (or ratio) sample stream.
class TimingFilter {
public:
    explicit TimingFilter(f32 alpha = 0.2f) : m_alpha(alpha) {}
    void add(f32 sample);
    void reset() { m_valid = false, m_value = 0.0f, m_samples = 0; }
    [[nodiscard]] bool valid() const { return m_valid; }
    [[nodiscard]] f32 value(f32 fallback = 0.0f) const { return m_valid ? m_value : fallback; }
    [[nodiscard]] u32 samples() const { return m_samples; }

private:
    f32 m_alpha;
    f32 m_value = 0.0f;
    bool m_valid = false;
    u32 m_samples = 0;
};

// Hysteresis over successive decisions: a placement change is applied only after `confirmations` consecutive
// evaluations agree on it (prevents flip-flopping when gains are close to the threshold or timings are noisy).
class SplitController {
public:
    explicit SplitController(u32 confirmations = 3) : m_confirmations(confirmations) {}
    // Returns the placement to use (true = offload), per segment index.
    const std::vector<bool>& update(const SplitDecision& decision);
    [[nodiscard]] const std::vector<bool>& placement() const { return m_applied; }
    void reset();

private:
    u32 m_confirmations;
    std::vector<bool> m_applied, m_pending;
    u32 m_agree = 0;
};

} // namespace ox::rhi
