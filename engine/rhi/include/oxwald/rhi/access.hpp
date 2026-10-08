#pragma once

#include <oxwald/rhi/types.hpp>

namespace ox::rhi {

// How a pass / command uses a resource. Maps to (stage, access, layout) for synchronization2 barriers.
// "Graphics" = vertex + fragment (and mesh/task) shader stages.
enum class Access : u8 {
    Undefined, // contents discarded / not yet used
    VertexBuffer,
    IndexBuffer,
    IndirectBuffer,
    UniformGraphics,
    UniformCompute,
    SampledGraphics,
    SampledFragment,
    SampledCompute,
    SampledRayTracing,
    StorageReadGraphics,
    StorageReadCompute,
    StorageReadRayTracing,
    StorageWriteGraphics, // read-write storage access
    StorageWriteCompute,
    StorageWriteRayTracing,
    ColorAttachmentWrite, // includes read for blending / LOAD
    DepthStencilWrite,
    DepthStencilRead, // read-only depth test, may also be sampled
    TransferRead,
    TransferWrite,
    Present,
    HostRead,
    HostWrite,
    AccelStructBuildRead,  // AS build inputs (vertex/index/instance buffers) and source AS for updates
    AccelStructBuildWrite, // AS destination + scratch
    AccelStructRead,       // ray query / trace rays in any shader stage
    General,               // everything; use sparingly
    Count
};

struct AccessInfo {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool write = false;
};

AccessInfo accessInfo(Access access);
const char* accessName(Access access);
inline bool isWrite(Access a) { return accessInfo(a).write; }

} // namespace ox::rhi
