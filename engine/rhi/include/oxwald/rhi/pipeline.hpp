#pragma once

#include <oxwald/rhi/shader_compiler.hpp>
#include <oxwald/rhi/types.hpp>

#include <vector>

namespace ox::rhi {

// Engine-wide pipeline layout: set 0 = bindless heap, push constants of this size for every stage.
inline constexpr u32 kMaxPushConstantSize = 128;

// A shader stage either from a GLSL file (hot reloadable), inline GLSL, or precompiled SPIR-V.
struct ShaderStageDesc {
    std::filesystem::path path;
    std::string source;
    std::vector<u32> spirv;
    ShaderStage stage = ShaderStage::Unknown;
    std::string entryPoint = "main";
    std::vector<ShaderDefine> defines;

    static ShaderStageDesc file(std::filesystem::path p, std::vector<ShaderDefine> defs = {}) {
        ShaderStageDesc d;
        d.path = std::move(p);
        d.defines = std::move(defs);
        return d;
    }
    static ShaderStageDesc glsl(std::string src, ShaderStage stage, std::string name = "inline") {
        ShaderStageDesc d;
        d.source = std::move(src);
        d.stage = stage;
        d.path = std::move(name);
        return d;
    }
    [[nodiscard]] bool empty() const { return path.empty() && source.empty() && spirv.empty(); }
};

struct SpecializationConstant {
    u32 id = 0;
    u32 value = 0; // raw 32-bit value (use std::bit_cast for floats)
};

struct VertexBinding {
    u32 binding = 0;
    u32 stride = 0;
    bool perInstance = false;
};

struct VertexAttribute {
    u32 location = 0;
    u32 binding = 0;
    VkFormat format = VK_FORMAT_R32G32B32_SFLOAT;
    u32 offset = 0;
};

struct RasterState {
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool depthClamp = false;
    bool depthBias = false; // values set dynamically via CommandList::setDepthBias
    f32 lineWidth = 1.f;
};

struct DepthState {
    bool test = false;
    bool write = false;
    VkCompareOp compare = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed-Z
    bool stencil = false;
    VkStencilOpState front{};
    VkStencilOpState back{};
};

struct BlendState {
    bool enable = false;
    VkBlendFactor srcColor = VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor dstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendOp colorOp = VK_BLEND_OP_ADD;
    VkBlendFactor srcAlpha = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dstAlpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendOp alphaOp = VK_BLEND_OP_ADD;
    VkColorComponentFlags writeMask = 0xF;

    static BlendState opaque() { return {}; }
    static BlendState alpha() { BlendState b; b.enable = true; return b; }
    static BlendState premultiplied() {
        BlendState b;
        b.enable = true;
        b.srcColor = VK_BLEND_FACTOR_ONE;
        return b;
    }
    static BlendState additive() {
        BlendState b;
        b.enable = true;
        b.srcColor = b.dstColor = b.srcAlpha = b.dstAlpha = VK_BLEND_FACTOR_ONE;
        return b;
    }
};

struct GraphicsPipelineDesc {
    std::string name;
    ShaderStageDesc vertex;   // or task/mesh for mesh pipelines
    ShaderStageDesc fragment; // may be empty (depth-only)
    ShaderStageDesc task;
    ShaderStageDesc mesh;
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
    // Vertex input is optional: the engine prefers vertex pulling through buffer device addresses.
    std::vector<VertexBinding> vertexBindings;
    std::vector<VertexAttribute> vertexAttributes;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    RasterState raster;
    DepthState depth;
    std::vector<BlendState> blend; // per color attachment; missing entries = opaque
    u32 samples = 1;
    u32 viewMask = 0; // multiview
    std::vector<SpecializationConstant> specialization;
    u32 pushConstantSize = 0; // > 0: validated against the shader's push-constant block
};

struct ComputePipelineDesc {
    std::string name;
    ShaderStageDesc shader;
    std::vector<SpecializationConstant> specialization;
    u32 pushConstantSize = 0;
};

struct RayTracingHitGroup {
    ShaderStageDesc closestHit;
    ShaderStageDesc anyHit;
    ShaderStageDesc intersection; // procedural geometry
};

struct RayTracingPipelineDesc {
    std::string name;
    ShaderStageDesc rayGen;
    std::vector<ShaderStageDesc> miss;
    std::vector<RayTracingHitGroup> hitGroups;
    std::vector<ShaderStageDesc> callables;
    u32 maxRecursionDepth = 1;
    std::vector<SpecializationConstant> specialization;
    u32 pushConstantSize = 0;
};

enum class PipelineKind : u8 { Graphics, Compute, RayTracing };

} // namespace ox::rhi
