#include "device_impl.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/rhi/format.hpp>

#include <algorithm>
#include <cstring>
#include <set>

namespace ox::rhi {

namespace fs = std::filesystem;
using namespace detail;

namespace {

struct CompiledStage {
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
    std::string entry;
    ShaderReflection reflection;
};

struct StageSet {
    DeviceState& s;
    std::vector<CompiledStage> stages;
    std::vector<fs::path> deps;
    std::string error;
    explicit StageSet(DeviceState& st) : s(st) {}
    ~StageSet() {
        for (auto& c : stages) vkDestroyShaderModule(s.device, c.module, nullptr);
    }

    // Returns index into `stages` or -1.
    i32 add(const ShaderStageDesc& d, ShaderStage expected, u32 pushConstantLimit) {
        CompiledStage out;
        std::vector<u32> words;
        if (!d.spirv.empty()) {
            words = d.spirv;
            std::string e;
            auto r = reflectSpirv(words, &e);
            if (!r) {
                error = e;
                return -1;
            }
            out.reflection = std::move(*r);
        } else {
            ShaderCompileDesc cd;
            cd.path = d.path;
            cd.source = d.source;
            cd.stage = d.stage != ShaderStage::Unknown ? d.stage : expected;
            if (!d.path.empty() && d.stage == ShaderStage::Unknown && shaderStageFromPath(d.path) != ShaderStage::Unknown) {
                cd.stage = shaderStageFromPath(d.path);
            }
            cd.entryPoint = d.entryPoint;
            cd.defines = d.defines;
            ShaderCompileResult res = s.compiler->compile(cd);
            deps.insert(deps.end(), res.dependencies.begin(), res.dependencies.end());
            if (!res.success) {
                error = std::format("{}: {}", d.path.string(), res.errors);
                return -1;
            }
            words = std::move(res.spirv);
            out.reflection = std::move(res.reflection);
        }
        std::string verr;
        if (!validateAgainstBindlessLayout(out.reflection, pushConstantLimit, verr)) {
            error = std::format("{}: {}", d.path.string(), verr);
            return -1;
        }
        const ShaderStage st = d.stage != ShaderStage::Unknown ? d.stage
                               : out.reflection.stage != ShaderStage::Unknown ? out.reflection.stage : expected;
        out.stage = toVkShaderStage(st);
        out.entry = d.entryPoint;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = words.size() * 4;
        ci.pCode = words.data();
        if (VkResult r = vkCreateShaderModule(s.device, &ci, nullptr, &out.module); r != VK_SUCCESS) {
            error = std::format("vkCreateShaderModule: {}", vkResultName(r));
            return -1;
        }
        stages.push_back(std::move(out));
        return i32(stages.size() - 1);
    }
};

struct Specialization {
    std::vector<VkSpecializationMapEntry> entries;
    std::vector<u32> data;
    VkSpecializationInfo info{};
    explicit Specialization(const std::vector<SpecializationConstant>& c) {
        for (u32 i = 0; i < c.size(); ++i) {
            entries.push_back({c[i].id, i * 4, 4});
            data.push_back(c[i].value);
        }
        info.mapEntryCount = u32(entries.size());
        info.pMapEntries = entries.data();
        info.dataSize = data.size() * 4;
        info.pData = data.data();
    }
    const VkSpecializationInfo* get() const { return entries.empty() ? nullptr : &info; }
};

u32 pushLimit(const DeviceState& s, u32 declared) {
    const u32 limit = std::min(kMaxPushConstantSize, s.caps.maxPushConstantsSize);
    return declared > 0 ? std::min(declared, limit) : limit;
}

VkPipelineShaderStageCreateInfo stageInfo(const CompiledStage& c, const VkSpecializationInfo* spec) {
    VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    si.stage = c.stage;
    si.module = c.module;
    si.pName = c.entry.c_str();
    si.pSpecializationInfo = spec;
    return si;
}

VkPipeline buildGraphics(Device& device, DeviceState& s, PipelineRecord& rec) {
    const GraphicsPipelineDesc& d = rec.graphics;
    StageSet set(s);
    const u32 limit = pushLimit(s, d.pushConstantSize);
    const bool meshPipeline = !d.mesh.empty();
    if (meshPipeline) {
        if (!s.caps.meshShader) {
            s.lastPipelineError = std::format("'{}': mesh shaders are not supported", d.name);
            return VK_NULL_HANDLE;
        }
        if (!d.task.empty() && set.add(d.task, ShaderStage::Task, limit) < 0) goto fail;
        if (set.add(d.mesh, ShaderStage::Mesh, limit) < 0) goto fail;
    } else if (set.add(d.vertex, ShaderStage::Vertex, limit) < 0) {
        goto fail;
    }
    if (!d.fragment.empty() && set.add(d.fragment, ShaderStage::Fragment, limit) < 0) goto fail;
    rec.dependencies = set.deps;
    {
        Specialization spec(d.specialization);
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        for (const auto& c : set.stages) stages.push_back(stageInfo(c, spec.get()));

        std::vector<VkVertexInputBindingDescription> bindings;
        for (const auto& b : d.vertexBindings) {
            bindings.push_back({b.binding, b.stride, b.perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        }
        std::vector<VkVertexInputAttributeDescription> attrs;
        for (const auto& a : d.vertexAttributes) attrs.push_back({a.location, a.binding, a.format, a.offset});
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = u32(bindings.size());
        vi.pVertexBindingDescriptions = bindings.data();
        vi.vertexAttributeDescriptionCount = u32(attrs.size());
        vi.pVertexAttributeDescriptions = attrs.data();

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = d.topology;

        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = s.caps.fillModeNonSolid ? d.raster.polygonMode : VK_POLYGON_MODE_FILL;
        rs.cullMode = d.raster.cullMode;
        rs.frontFace = d.raster.frontFace;
        rs.depthClampEnable = d.raster.depthClamp && s.caps.depthClamp;
        rs.depthBiasEnable = d.raster.depthBias;
        rs.lineWidth = s.caps.wideLines ? d.raster.lineWidth : 1.f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VkSampleCountFlagBits(std::max(1u, d.samples));

        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = d.depth.test;
        ds.depthWriteEnable = d.depth.write;
        ds.depthCompareOp = d.depth.compare;
        ds.stencilTestEnable = d.depth.stencil;
        ds.front = d.depth.front;
        ds.back = d.depth.back;
        ds.maxDepthBounds = 1.f;

        std::vector<VkPipelineColorBlendAttachmentState> blends;
        for (usize i = 0; i < d.colorFormats.size(); ++i) {
            const BlendState b = i < d.blend.size() ? d.blend[i] : BlendState{};
            VkPipelineColorBlendAttachmentState a{};
            a.blendEnable = b.enable;
            a.srcColorBlendFactor = b.srcColor;
            a.dstColorBlendFactor = b.dstColor;
            a.colorBlendOp = b.colorOp;
            a.srcAlphaBlendFactor = b.srcAlpha;
            a.dstAlphaBlendFactor = b.dstAlpha;
            a.alphaBlendOp = b.alphaOp;
            a.colorWriteMask = b.writeMask;
            blends.push_back(a);
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = u32(blends.size());
        cb.pAttachments = blends.data();

        std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        if (d.raster.depthBias) dyn.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
        if (d.depth.stencil) dyn.push_back(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = u32(dyn.size());
        dy.pDynamicStates = dyn.data();

        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.viewMask = d.viewMask;
        ri.colorAttachmentCount = u32(d.colorFormats.size());
        ri.pColorAttachmentFormats = d.colorFormats.data();
        ri.depthAttachmentFormat = formatInfo(d.depthFormat).depth ? d.depthFormat : VK_FORMAT_UNDEFINED;
        ri.stencilAttachmentFormat = d.stencilFormat != VK_FORMAT_UNDEFINED ? d.stencilFormat
                                     : formatInfo(d.depthFormat).stencil ? d.depthFormat : VK_FORMAT_UNDEFINED;
        if (d.depthFormat == VK_FORMAT_UNDEFINED) ri.depthAttachmentFormat = VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &ri;
        ci.stageCount = u32(stages.size());
        ci.pStages = stages.data();
        ci.pVertexInputState = meshPipeline ? nullptr : &vi;
        ci.pInputAssemblyState = meshPipeline ? nullptr : &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = s.pipelineLayout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (VkResult r = vkCreateGraphicsPipelines(s.device, s.pipelineCache, 1, &ci, nullptr, &pipeline); r != VK_SUCCESS) {
            s.lastPipelineError = std::format("'{}': vkCreateGraphicsPipelines: {}", d.name, vkResultName(r));
            return VK_NULL_HANDLE;
        }
        device.setDebugName(VK_OBJECT_TYPE_PIPELINE, u64(pipeline), d.name);
        return pipeline;
    }
fail:
    rec.dependencies = set.deps;
    s.lastPipelineError = std::format("'{}': {}", d.name, set.error);
    return VK_NULL_HANDLE;
}

VkPipeline buildCompute(Device& device, DeviceState& s, PipelineRecord& rec) {
    const ComputePipelineDesc& d = rec.compute;
    StageSet set(s);
    const i32 idx = set.add(d.shader, ShaderStage::Compute, pushLimit(s, d.pushConstantSize));
    rec.dependencies = set.deps;
    if (idx < 0) {
        s.lastPipelineError = std::format("'{}': {}", d.name, set.error);
        return VK_NULL_HANDLE;
    }
    const auto& ls = set.stages[0].reflection.localSize;
    rec.localSize = {std::max(1u, ls[0]), std::max(1u, ls[1]), std::max(1u, ls[2])};
    Specialization spec(d.specialization);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stageInfo(set.stages[0], spec.get());
    ci.layout = s.pipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (VkResult r = vkCreateComputePipelines(s.device, s.pipelineCache, 1, &ci, nullptr, &pipeline); r != VK_SUCCESS) {
        s.lastPipelineError = std::format("'{}': vkCreateComputePipelines: {}", d.name, vkResultName(r));
        return VK_NULL_HANDLE;
    }
    device.setDebugName(VK_OBJECT_TYPE_PIPELINE, u64(pipeline), d.name);
    return pipeline;
}

u64 alignUp(u64 v, u64 a) { return a ? (v + a - 1) / a * a : v; }

VkPipeline buildRayTracing(Device& device, DeviceState& s, PipelineRecord& rec, BufferHandle& sbtOut,
                           VkStridedDeviceAddressRegionKHR regions[4]) {
    const RayTracingPipelineDesc& d = rec.rayTracing;
    if (!s.caps.rayTracingPipeline) {
        s.lastPipelineError = std::format("'{}': ray tracing pipelines are not supported on this device", d.name);
        return VK_NULL_HANDLE;
    }
    StageSet set(s);
    const u32 limit = pushLimit(s, d.pushConstantSize);
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
    auto general = [&](i32 stage) {
        VkRayTracingShaderGroupCreateInfoKHR g{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        g.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        g.generalShader = u32(stage);
        g.closestHitShader = g.anyHitShader = g.intersectionShader = VK_SHADER_UNUSED_KHR;
        groups.push_back(g);
    };
    i32 idx = set.add(d.rayGen, ShaderStage::RayGen, limit);
    if (idx < 0) goto fail;
    general(idx);
    for (const auto& m : d.miss) {
        if ((idx = set.add(m, ShaderStage::Miss, limit)) < 0) goto fail;
        general(idx);
    }
    for (const auto& h : d.hitGroups) {
        VkRayTracingShaderGroupCreateInfoKHR g{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        g.type = h.intersection.empty() ? VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR
                                        : VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
        g.generalShader = g.closestHitShader = g.anyHitShader = g.intersectionShader = VK_SHADER_UNUSED_KHR;
        if (!h.closestHit.empty()) {
            if ((idx = set.add(h.closestHit, ShaderStage::ClosestHit, limit)) < 0) goto fail;
            g.closestHitShader = u32(idx);
        }
        if (!h.anyHit.empty()) {
            if ((idx = set.add(h.anyHit, ShaderStage::AnyHit, limit)) < 0) goto fail;
            g.anyHitShader = u32(idx);
        }
        if (!h.intersection.empty()) {
            if ((idx = set.add(h.intersection, ShaderStage::Intersection, limit)) < 0) goto fail;
            g.intersectionShader = u32(idx);
        }
        groups.push_back(g);
    }
    for (const auto& c : d.callables) {
        if ((idx = set.add(c, ShaderStage::Callable, limit)) < 0) goto fail;
        general(idx);
    }
    rec.dependencies = set.deps;
    {
        Specialization spec(d.specialization);
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        for (const auto& c : set.stages) stages.push_back(stageInfo(c, spec.get()));
        VkRayTracingPipelineCreateInfoKHR ci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        ci.stageCount = u32(stages.size());
        ci.pStages = stages.data();
        ci.groupCount = u32(groups.size());
        ci.pGroups = groups.data();
        ci.maxPipelineRayRecursionDepth = std::min(d.maxRecursionDepth, std::max(1u, s.caps.maxRayRecursionDepth));
        ci.layout = s.pipelineLayout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (VkResult r = vkCreateRayTracingPipelinesKHR(s.device, VK_NULL_HANDLE, s.pipelineCache, 1, &ci, nullptr, &pipeline);
            r != VK_SUCCESS) {
            s.lastPipelineError = std::format("'{}': vkCreateRayTracingPipelinesKHR: {}", d.name, vkResultName(r));
            return VK_NULL_HANDLE;
        }
        device.setDebugName(VK_OBJECT_TYPE_PIPELINE, u64(pipeline), d.name);

        // Shader binding table: [raygen][miss...][hit...][callable...], each region base-aligned.
        const u32 handleSize = s.caps.shaderGroupHandleSize;
        const u64 stride = alignUp(handleSize, s.caps.shaderGroupHandleAlignment);
        const u64 base = s.caps.shaderGroupBaseAlignment;
        const u32 counts[4] = {1, u32(d.miss.size()), u32(d.hitGroups.size()), u32(d.callables.size())};
        u64 sizes[4], offsets[4], total = 0;
        for (u32 i = 0; i < 4; ++i) {
            sizes[i] = i == 0 ? alignUp(stride, base) : alignUp(counts[i] * stride, base);
            offsets[i] = total;
            total += sizes[i];
        }
        std::vector<u8> handles(groups.size() * handleSize);
        OX_VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(s.device, pipeline, 0, u32(groups.size()), handles.size(),
                                                         handles.data()));
        std::vector<u8> table(total, 0);
        u32 g = 0;
        for (u32 r = 0; r < 4; ++r) {
            for (u32 i = 0; i < counts[r]; ++i, ++g) {
                std::memcpy(table.data() + offsets[r] + i * stride, handles.data() + g * handleSize, handleSize);
            }
        }
        sbtOut = device.createBuffer({total, BufferUsage::ShaderBindingTable, MemoryUsage::Upload, d.name + ".sbt"}, table.data());
        const VkDeviceAddress addr = device.address(sbtOut);
        for (u32 r = 0; r < 4; ++r) {
            regions[r] = {counts[r] ? addr + offsets[r] : 0, counts[r] ? (r == 0 ? sizes[0] : stride) : 0,
                          counts[r] ? sizes[r] : 0};
        }
        return pipeline;
    }
fail:
    rec.dependencies = set.deps;
    s.lastPipelineError = std::format("'{}': {}", d.name, set.error);
    return VK_NULL_HANDLE;
}

VkPipeline buildPipeline(Device& device, DeviceState& s, PipelineRecord& rec, BufferHandle& sbt,
                         VkStridedDeviceAddressRegionKHR regions[4]) {
    switch (rec.kind) {
    case PipelineKind::Graphics: return buildGraphics(device, s, rec);
    case PipelineKind::Compute: return buildCompute(device, s, rec);
    case PipelineKind::RayTracing: return buildRayTracing(device, s, rec, sbt, regions);
    }
    return VK_NULL_HANDLE;
}

PipelineHandle createPipeline(Device& device, DeviceState& s, PipelineRecord rec) {
    rec.pipeline = buildPipeline(device, s, rec, rec.sbt, rec.regions);
    if (!rec.pipeline) {
        OX_LOG_ERROR("shader", "pipeline {}", s.lastPipelineError);
    } else {
        rec.version = 1;
    }
    for (const auto& d : rec.dependencies) s.shaderFiles.watch(d);
    std::lock_guard lock(s.resourceMutex);
    return s.pipelines.allocate(std::move(rec));
}

} // namespace

PipelineHandle Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    PipelineRecord rec;
    rec.kind = PipelineKind::Graphics;
    rec.name = desc.name;
    rec.graphics = desc;
    return createPipeline(*this, *m_s, std::move(rec));
}

PipelineHandle Device::createComputePipeline(const ComputePipelineDesc& desc) {
    PipelineRecord rec;
    rec.kind = PipelineKind::Compute;
    rec.name = desc.name;
    rec.compute = desc;
    return createPipeline(*this, *m_s, std::move(rec));
}

PipelineHandle Device::createRayTracingPipeline(const RayTracingPipelineDesc& desc) {
    PipelineRecord rec;
    rec.kind = PipelineKind::RayTracing;
    rec.name = desc.name;
    rec.rayTracing = desc;
    return createPipeline(*this, *m_s, std::move(rec));
}

void Device::destroy(PipelineHandle pipeline) {
    DeviceState& s = *m_s;
    std::optional<PipelineRecord> rec;
    {
        std::lock_guard lock(s.resourceMutex);
        rec = s.pipelines.release(pipeline);
    }
    if (!rec) return;
    if (rec->sbt) destroy(rec->sbt);
    VkPipeline p = rec->pipeline;
    s.retire([&s, p] { vkDestroyPipeline(s.device, p, nullptr); });
}

bool Device::isAlive(PipelineHandle p) const { return m_s->pipelines.get(p) != nullptr; }
VkPipeline Device::vkPipeline(PipelineHandle p) const {
    const PipelineRecord* r = m_s->pipelines.get(p);
    return r ? r->pipeline : VK_NULL_HANDLE;
}
PipelineKind Device::pipelineKind(PipelineHandle p) const { return m_s->pipelines.at(p).kind; }
u32 Device::pipelineVersion(PipelineHandle p) const { return m_s->pipelines.at(p).version; }

namespace detail {

u32 reloadShadersImpl(Device& device, DeviceState& s, bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - s.lastShaderPoll < std::chrono::milliseconds(s.desc.hotReloadPollMs)) return 0;
    s.lastShaderPoll = now;
    const std::vector<fs::path> changed = s.shaderFiles.poll();
    if (changed.empty()) return 0;
    std::set<std::string> changedSet;
    for (const auto& c : changed) changedSet.insert(c.lexically_normal().string());

    std::vector<PipelineHandle> affected;
    s.pipelines.forEach([&](PipelineHandle h, PipelineRecord& rec) {
        for (const auto& d : rec.dependencies) {
            if (changedSet.contains(d.lexically_normal().string())) {
                affected.push_back(h);
                break;
            }
        }
    });
    u32 rebuilt = 0;
    for (PipelineHandle h : affected) {
        PipelineRecord& rec = s.pipelines.at(h);
        PipelineRecord candidate = rec; // keep descs; build into a copy so failures leave `rec` untouched
        BufferHandle sbt;
        VkStridedDeviceAddressRegionKHR regions[4]{};
        VkPipeline p = buildPipeline(device, s, candidate, sbt, regions);
        for (const auto& d : candidate.dependencies) s.shaderFiles.watch(d);
        if (!p) {
            OX_LOG_ERROR("shader", "hot reload failed, keeping previous pipeline: {}", s.lastPipelineError);
            continue;
        }
        VkPipeline old = rec.pipeline;
        if (old) s.retire([&s, old] { vkDestroyPipeline(s.device, old, nullptr); });
        if (rec.sbt) device.destroy(rec.sbt);
        rec.pipeline = p;
        rec.dependencies = candidate.dependencies;
        rec.localSize = candidate.localSize;
        rec.sbt = sbt;
        std::copy(std::begin(regions), std::end(regions), std::begin(rec.regions));
        ++rec.version;
        ++rebuilt;
        OX_LOG_INFO("shader", "hot reloaded pipeline '{}' (v{})", rec.name, rec.version);
    }
    return rebuilt;
}

} // namespace detail

u32 Device::reloadChangedShaders(bool force) { return reloadShadersImpl(*this, *m_s, force); }

} // namespace ox::rhi
