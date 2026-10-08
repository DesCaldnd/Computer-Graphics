// Acceleration structures. Always compiled; only usable when DeviceCaps::accelerationStructure is set
// (the KHR entry points are null otherwise).
#include "device_impl.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>

#include <cstring>

namespace ox::rhi {

using namespace detail;

namespace {

u64 alignUp(u64 v, u64 a) { return a ? (v + a - 1) / a * a : v; }

struct BlasGeometryInfo {
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<u32> primitiveCounts;
};

BlasGeometryInfo describeBlas(Device& device, const BlasDesc& desc) {
    BlasGeometryInfo out;
    for (const BlasTriangles& t : desc.geometries) {
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = t.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        auto& tri = g.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = t.vertexFormat;
        tri.vertexData.deviceAddress = device.address(t.vertexBuffer) + t.vertexOffset;
        tri.vertexStride = t.vertexStride;
        tri.maxVertex = t.vertexCount ? t.vertexCount - 1 : 0;
        u32 primitives = t.vertexCount / 3;
        if (t.indexBuffer) {
            tri.indexType = t.indexType;
            tri.indexData.deviceAddress = device.address(t.indexBuffer) + t.indexOffset;
            primitives = t.indexCount / 3;
        } else {
            tri.indexType = VK_INDEX_TYPE_NONE_KHR;
        }
        out.geometries.push_back(g);
        out.ranges.push_back({primitives, 0, 0, 0});
        out.primitiveCounts.push_back(primitives);
    }
    return out;
}

VkAccelerationStructureKHR createAS(Device& device, DeviceState& s, VkAccelerationStructureTypeKHR type, u64 size,
                                    BufferHandle& buffer, u64& address, const std::string& name) {
    buffer = device.createBuffer({size, BufferUsage::AccelStructStorage, MemoryUsage::GpuOnly, name + ".storage"});
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = device.vkBuffer(buffer);
    ci.size = size;
    ci.type = type;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    OX_VK_CHECK(vkCreateAccelerationStructureKHR(s.device, &ci, nullptr, &as));
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = as;
    address = vkGetAccelerationStructureDeviceAddressKHR(s.device, &ai);
    device.setDebugName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, u64(as), name);
    return as;
}

BufferHandle scratchBuffer(Device& device, DeviceState& s, u64 size, const std::string& name) {
    const u64 align = std::max(1u, s.caps.minAccelerationStructureScratchOffsetAlignment);
    return device.createBuffer({alignUp(size, align) + align, BufferUsage::Storage, MemoryUsage::GpuOnly, name + ".scratch"});
}

VkDeviceAddress scratchAddress(Device& device, DeviceState& s, BufferHandle b) {
    return alignUp(device.address(b), std::max(1u, s.caps.minAccelerationStructureScratchOffsetAlignment));
}

} // namespace

AccelStructHandle Device::createBlas(const BlasDesc& desc) {
    DeviceState& s = *m_s;
    if (!s.caps.accelerationStructure) {
        OX_LOG_ERROR("rhi", "createBlas('{}'): {}", desc.name, s.caps.whyRayTracingUnavailable());
        return {};
    }
    BlasGeometryInfo geo = describeBlas(*this, desc);
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = (desc.preferFastBuild ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                                        : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) |
                  (desc.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0) |
                  (desc.allowCompaction ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0);
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = u32(geo.geometries.size());
    build.pGeometries = geo.geometries.data();
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(s.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                                            geo.primitiveCounts.data(), &sizes);

    AccelStructRecord rec;
    rec.blas = desc;
    rec.handle = createAS(*this, s, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize, rec.buffer,
                          rec.address, desc.name);
    rec.scratchSize = std::max(sizes.buildScratchSize, sizes.updateScratchSize);
    BufferHandle scratch = scratchBuffer(*this, s, rec.scratchSize, desc.name);
    build.dstAccelerationStructure = rec.handle;
    build.scratchData.deviceAddress = scratchAddress(*this, s, scratch);

    VkQueryPool query = VK_NULL_HANDLE;
    if (desc.allowCompaction && !desc.allowUpdate) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        qi.queryCount = 1;
        OX_VK_CHECK(vkCreateQueryPool(s.device, &qi, nullptr, &query));
        vkResetQueryPool(s.device, query, 0, 1);
    }
    immediateSubmit([&](CommandList& cmd) {
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = geo.ranges.data();
        vkCmdBuildAccelerationStructuresKHR(cmd.vk(), 1, &build, &ranges);
        if (query) {
            cmd.memoryBarrier(Access::AccelStructBuildWrite, Access::AccelStructBuildRead);
            vkCmdWriteAccelerationStructuresPropertiesKHR(cmd.vk(), 1, &rec.handle,
                                                          VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, query, 0);
        }
    });

    if (query) {
        u64 compacted = 0;
        vkGetQueryPoolResults(s.device, query, 0, 1, sizeof(u64), &compacted, sizeof(u64),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        vkDestroyQueryPool(s.device, query, nullptr);
        if (compacted > 0 && compacted < sizes.accelerationStructureSize) {
            BufferHandle compactBuffer;
            u64 compactAddress = 0;
            VkAccelerationStructureKHR compact = createAS(*this, s, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                          compacted, compactBuffer, compactAddress, desc.name);
            immediateSubmit([&](CommandList& cmd) {
                VkCopyAccelerationStructureInfoKHR ci{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
                ci.src = rec.handle;
                ci.dst = compact;
                ci.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                vkCmdCopyAccelerationStructureKHR(cmd.vk(), &ci);
            });
            vkDestroyAccelerationStructureKHR(s.device, rec.handle, nullptr);
            destroy(rec.buffer);
            rec.handle = compact;
            rec.buffer = compactBuffer;
            rec.address = compactAddress;
        }
    }
    if (desc.allowUpdate) {
        rec.scratch = scratch; // kept for refits
    } else {
        destroy(scratch);
    }
    std::lock_guard lock(s.resourceMutex);
    return s.accelStructs.allocate(std::move(rec));
}

AccelStructHandle Device::createTlas(const TlasDesc& desc) {
    DeviceState& s = *m_s;
    if (!s.caps.accelerationStructure) {
        OX_LOG_ERROR("rhi", "createTlas('{}'): {}", desc.name, s.caps.whyRayTracingUnavailable());
        return {};
    }
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                  (desc.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    build.geometryCount = 1;
    build.pGeometries = &g;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(s.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                                            &desc.maxInstances, &sizes);
    AccelStructRecord rec;
    rec.tlas = true;
    rec.tlasDesc = desc;
    rec.handle = createAS(*this, s, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizes.accelerationStructureSize, rec.buffer,
                          rec.address, desc.name);
    rec.scratchSize = std::max(sizes.buildScratchSize, sizes.updateScratchSize);
    rec.scratch = scratchBuffer(*this, s, rec.scratchSize, desc.name);
    rec.instances = createBuffer({u64(desc.maxInstances) * sizeof(VkAccelerationStructureInstanceKHR) * framesInFlight(),
                                  BufferUsage::AccelStructInput, MemoryUsage::Dynamic, desc.name + ".instances"});
    std::lock_guard lock(s.resourceMutex);
    return s.accelStructs.allocate(std::move(rec));
}

void Device::destroy(AccelStructHandle h) {
    DeviceState& s = *m_s;
    std::optional<AccelStructRecord> rec;
    {
        std::lock_guard lock(s.resourceMutex);
        rec = s.accelStructs.release(h);
    }
    if (!rec) return;
    VkAccelerationStructureKHR as = rec->handle;
    s.retire([&s, as] { vkDestroyAccelerationStructureKHR(s.device, as, nullptr); });
    destroy(rec->buffer);
    if (rec->scratch) destroy(rec->scratch);
    if (rec->instances) destroy(rec->instances);
}

u64 Device::accelStructAddress(AccelStructHandle h) const { return m_s->accelStructs.at(h).address; }
VkAccelerationStructureKHR Device::vkAccelStruct(AccelStructHandle h) const { return m_s->accelStructs.at(h).handle; }

void CommandList::buildTlas(AccelStructHandle tlas, std::span<const TlasInstance> instances, bool update) {
    DeviceState& s = m_device->state();
    OX_ASSERT(s.caps.accelerationStructure, "acceleration structures are not supported");
    AccelStructRecord& rec = s.accelStructs.at(tlas);
    OX_ASSERT(rec.tlas && instances.size() <= rec.tlasDesc.maxInstances, "TLAS '{}' holds at most {} instances",
              rec.tlasDesc.name, rec.tlasDesc.maxInstances);
    const u64 regionSize = u64(rec.tlasDesc.maxInstances) * sizeof(VkAccelerationStructureInstanceKHR);
    const u64 regionOffset = regionSize * m_device->frameIndex();
    auto* dst = static_cast<VkAccelerationStructureInstanceKHR*>(m_device->mapped(rec.instances)) +
                regionOffset / sizeof(VkAccelerationStructureInstanceKHR);
    for (usize i = 0; i < instances.size(); ++i) {
        const TlasInstance& in = instances[i];
        VkAccelerationStructureInstanceKHR out{};
        // glm::mat3x4 is 3 columns of vec4; VkTransformMatrixKHR is 3 rows of 4 floats (row-major 3x4).
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) out.transform.matrix[r][c] = in.transform[r][c];
        }
        out.instanceCustomIndex = in.customIndex & 0xFFFFFF;
        out.mask = in.mask;
        out.instanceShaderBindingTableRecordOffset = in.sbtRecordOffset & 0xFFFFFF;
        out.flags = in.flags;
        out.accelerationStructureReference = m_device->accelStructAddress(in.blas);
        dst[i] = out;
    }
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.data.deviceAddress = m_device->address(rec.instances) + regionOffset;
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                  (rec.tlasDesc.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    const bool doUpdate = update && rec.tlasDesc.allowUpdate && rec.builtInstances == instances.size();
    build.mode = doUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.srcAccelerationStructure = doUpdate ? rec.handle : VK_NULL_HANDLE;
    build.dstAccelerationStructure = rec.handle;
    build.geometryCount = 1;
    build.pGeometries = &g;
    build.scratchData.deviceAddress = scratchAddress(*m_device, s, rec.scratch);
    const VkAccelerationStructureBuildRangeInfoKHR range{u32(instances.size()), 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    memoryBarrier(Access::HostWrite, Access::AccelStructBuildRead);
    vkCmdBuildAccelerationStructuresKHR(m_cmd, 1, &build, &ranges);
    memoryBarrier(Access::AccelStructBuildWrite, Access::AccelStructRead);
    rec.builtInstances = u32(instances.size());
}

void CommandList::refitBlas(AccelStructHandle blas) {
    DeviceState& s = m_device->state();
    AccelStructRecord& rec = s.accelStructs.at(blas);
    OX_ASSERT(!rec.tlas && rec.blas.allowUpdate && rec.scratch, "refitBlas needs a BLAS created with allowUpdate");
    BlasGeometryInfo geo = describeBlas(*m_device, rec.blas);
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                  (rec.blas.preferFastBuild ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                                            : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    build.srcAccelerationStructure = rec.handle;
    build.dstAccelerationStructure = rec.handle;
    build.geometryCount = u32(geo.geometries.size());
    build.pGeometries = geo.geometries.data();
    build.scratchData.deviceAddress = scratchAddress(*m_device, s, rec.scratch);
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = geo.ranges.data();
    memoryBarrier(Access::StorageWriteCompute, Access::AccelStructBuildRead);
    vkCmdBuildAccelerationStructuresKHR(m_cmd, 1, &build, &ranges);
    memoryBarrier(Access::AccelStructBuildWrite, Access::AccelStructRead);
}

} // namespace ox::rhi
