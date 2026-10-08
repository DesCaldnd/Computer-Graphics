#include <oxwald/rhi/access.hpp>

namespace ox::rhi {

namespace {
constexpr VkPipelineStageFlags2 kGraphicsShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 kRayTracing = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
constexpr VkPipelineStageFlags2 kDepthTests =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
} // namespace

AccessInfo accessInfo(Access a) {
    switch (a) {
    case Access::Undefined: return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::VertexBuffer:
        return {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::IndexBuffer:
        return {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::IndirectBuffer:
        return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::UniformGraphics: return {kGraphicsShaders, VK_ACCESS_2_UNIFORM_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::UniformCompute: return {kCompute, VK_ACCESS_2_UNIFORM_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::SampledGraphics:
        return {kGraphicsShaders, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false};
    case Access::SampledFragment:
        return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false};
    case Access::SampledCompute: return {kCompute, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false};
    case Access::SampledRayTracing:
        return {kRayTracing, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false};
    case Access::StorageReadGraphics: return {kGraphicsShaders, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, false};
    case Access::StorageReadCompute: return {kCompute, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, false};
    case Access::StorageReadRayTracing: return {kRayTracing, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, false};
    case Access::StorageWriteGraphics:
        return {kGraphicsShaders, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, true};
    case Access::StorageWriteCompute:
        return {kCompute, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, true};
    case Access::StorageWriteRayTracing:
        return {kRayTracing, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, true};
    case Access::ColorAttachmentWrite:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true};
    case Access::DepthStencilWrite:
        return {kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true};
    case Access::DepthStencilRead:
        return {kDepthTests | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, false};
    case Access::TransferRead:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false};
    case Access::TransferWrite:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true};
    case Access::Present: return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, false};
    case Access::HostRead: return {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, false};
    case Access::HostWrite: return {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, true};
    case Access::AccelStructBuildRead:
        return {VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::AccelStructBuildWrite:
        return {VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_IMAGE_LAYOUT_UNDEFINED, true};
    case Access::AccelStructRead:
        return {kGraphicsShaders | kCompute | kRayTracing, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                VK_IMAGE_LAYOUT_UNDEFINED, false};
    case Access::General:
        return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL, true};
    case Access::Count: break;
    }
    return {};
}

const char* accessName(Access a) {
    static constexpr const char* kNames[] = {
        "Undefined",           "VertexBuffer",         "IndexBuffer",           "IndirectBuffer",      "UniformGraphics",
        "UniformCompute",      "SampledGraphics",      "SampledFragment",       "SampledCompute",      "SampledRayTracing",
        "StorageReadGraphics", "StorageReadCompute",   "StorageReadRayTracing", "StorageWriteGraphics", "StorageWriteCompute",
        "StorageWriteRayTracing", "ColorAttachmentWrite", "DepthStencilWrite",  "DepthStencilRead",    "TransferRead",
        "TransferWrite",       "Present",              "HostRead",              "HostWrite",           "AccelStructBuildRead",
        "AccelStructBuildWrite", "AccelStructRead",    "General"};
    static_assert(std::size(kNames) == size_t(Access::Count));
    return a < Access::Count ? kNames[size_t(a)] : "?";
}

} // namespace ox::rhi
