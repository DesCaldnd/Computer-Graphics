#pragma once

#include <oxwald/rhi/types.hpp>

#include <glm/glm.hpp>

#include <vector>

namespace ox::rhi {

// Triangle geometry for a bottom-level acceleration structure. Buffers need BufferUsage::AccelStructInput.
struct BlasTriangles {
    BufferHandle vertexBuffer;
    u64 vertexOffset = 0;
    u32 vertexCount = 0;
    u32 vertexStride = 12;
    VkFormat vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    BufferHandle indexBuffer; // optional (non-indexed if null)
    u64 indexOffset = 0;
    u32 indexCount = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT32;
    bool opaque = true;
};

struct BlasDesc {
    std::string name;
    std::vector<BlasTriangles> geometries;
    bool allowUpdate = false;   // refit for skinned/deformed meshes
    bool allowCompaction = true; // static meshes: build → query compacted size → copy
    bool preferFastBuild = false;
};

struct TlasInstance {
    glm::mat3x4 transform{1.f}; // row-major 3x4 (VkTransformMatrixKHR)
    u32 customIndex = 0;        // 24 bits, gl_InstanceCustomIndexEXT
    u8 mask = 0xFF;
    u32 sbtRecordOffset = 0;    // 24 bits
    VkGeometryInstanceFlagsKHR flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    AccelStructHandle blas;
};

struct TlasDesc {
    std::string name;
    u32 maxInstances = 1024;
    bool allowUpdate = true;
};

} // namespace ox::rhi
