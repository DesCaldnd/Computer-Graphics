#pragma once

#include <oxwald/core/types.hpp>

#include <string>
#include <vector>

namespace ox::rhi {

// GPU time of one timestamp scope (render graph pass or CommandList::beginTimestamp/endTimestamp).
struct GpuTiming {
    std::string name;
    f64 milliseconds = 0.0;
    u32 depth = 0;
    // Start relative to the earliest scope of the frame (all queues share the device time domain): overlap of
    // async-compute passes with graphics work = intersecting [startMs, startMs + milliseconds) ranges.
    f64 startMs = 0.0;
    u8 queue = 0; // QueueType
};

struct GpuMemoryHeapStats {
    u64 budgetBytes = 0;
    u64 usageBytes = 0;
    u64 allocationBytes = 0; // bytes in live VMA allocations
    u64 blockBytes = 0;      // bytes in VkDeviceMemory blocks
    u32 allocationCount = 0;
    bool deviceLocal = false;
};

struct GpuMemoryStats {
    std::vector<GpuMemoryHeapStats> heaps;
    u64 totalUsageBytes = 0;
    u64 totalBudgetBytes = 0;
    u32 bufferCount = 0;
    u32 textureCount = 0;
};

// RenderDoc in-application API. The library is only loaded if RenderDoc injected itself or is installed
// (Windows/Linux). RenderDoc does not exist on macOS; use Xcode's GPU frame capture (MTL_CAPTURE_ENABLED=1).
class RenderDocCapture {
public:
    // Tries to attach to an injected RenderDoc, or loads it when `loadIfMissing` is set.
    bool initialize(bool loadIfMissing = false);
    [[nodiscard]] bool available() const { return m_api != nullptr; }
    [[nodiscard]] const std::string& unavailableReason() const { return m_reason; }

    void triggerCapture();               // captures the next presented frame
    void startFrameCapture();            // explicit capture of headless work
    bool endFrameCapture();
    [[nodiscard]] bool isFrameCapturing() const;

private:
    void* m_api = nullptr;
    std::string m_reason = "not initialized";
};

} // namespace ox::rhi
