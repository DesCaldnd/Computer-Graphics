#pragma once

#include <oxwald/rhi/types.hpp>

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ox::rhi {

enum class ShaderStage : u8 {
    Unknown,
    Vertex,
    Fragment,
    Compute,
    Geometry,
    TessControl,
    TessEval,
    Task,
    Mesh,
    RayGen,
    Miss,
    ClosestHit,
    AnyHit,
    Intersection,
    Callable,
};

VkShaderStageFlagBits toVkShaderStage(ShaderStage stage);
const char* shaderStageName(ShaderStage stage);
// From the extension: foo.vert / foo.vert.glsl, .frag, .comp, .geom, .tesc, .tese, .task, .mesh, .rgen, .rmiss,
// .rchit, .rahit, .rint, .rcall.
ShaderStage shaderStageFromPath(const std::filesystem::path& path);

struct ShaderDefine {
    std::string name;
    std::string value = "1";
    bool operator==(const ShaderDefine&) const = default;
};

struct ShaderReflection {
    struct Binding {
        u32 set = 0;
        u32 binding = 0;
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        u32 count = 1; // 0 = runtime array
        std::string name;
    };
    ShaderStage stage = ShaderStage::Unknown;
    std::string entryPoint;
    u32 pushConstantSize = 0;
    std::array<u32, 3> localSize{0, 0, 0}; // compute / mesh / task
    std::vector<Binding> bindings;
};

// Reflects SPIR-V with spirv-reflect. Returns nullopt (and fills error) for malformed modules.
std::optional<ShaderReflection> reflectSpirv(std::span<const u32> spirv, std::string* error = nullptr);

struct ShaderCompileDesc {
    std::filesystem::path path; // absolute, relative to cwd, or relative to an include root
    std::string source;         // optional inline GLSL; `path` is then only a display name / base for includes
    ShaderStage stage = ShaderStage::Unknown; // Unknown = derived from the path extension
    std::string entryPoint = "main";
    std::vector<ShaderDefine> defines;
};

struct ShaderCompileResult {
    bool success = false;
    std::vector<u32> spirv;
    std::vector<std::filesystem::path> dependencies; // main file + every transitively included file
    ShaderReflection reflection;
    std::string errors; // compiler / validation messages (also warnings on success)
    bool fromCache = false;
    u64 cacheKey = 0;
};

struct ShaderCompilerOptions {
    std::vector<std::filesystem::path> includeRoots; // searched after the including file's directory
    bool addEngineShaderRoot = true;                 // engine/shaders (dev) or <exe>/shaders (packaged)
    std::filesystem::path cacheDirectory;            // empty = no SPIR-V disk cache
    bool optimize = true;   // default: on in release builds (see OX_RHI_SHADER_OPTIMIZE)
    bool debugInfo = true;  // default: on in debug/dev builds — source-level debugging in RenderDoc
    std::vector<ShaderDefine> globalDefines;

    static ShaderCompilerOptions defaults();
};

// GLSL 4.60 → SPIR-V 1.5 (Vulkan 1.2 environment) via shaderc. Thread-safe.
class ShaderCompiler {
public:
    explicit ShaderCompiler(ShaderCompilerOptions options = ShaderCompilerOptions::defaults());
    ~ShaderCompiler();
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    ShaderCompileResult compile(const ShaderCompileDesc& desc);

    // `#include "x"` searches the requesting file's directory first, then the roots; `#include <x>` only roots.
    std::optional<std::filesystem::path> resolveInclude(std::string_view requested, const std::filesystem::path& requester,
                                                        bool relative) const;
    std::optional<std::filesystem::path> resolveSourcePath(const std::filesystem::path& path) const;

    [[nodiscard]] const ShaderCompilerOptions& options() const { return m_options; }
    [[nodiscard]] const std::vector<std::filesystem::path>& includeRoots() const { return m_roots; }
    [[nodiscard]] u64 cacheHits() const { return m_cacheHits.load(); }
    [[nodiscard]] u64 cacheMisses() const { return m_cacheMisses.load(); }

    static std::filesystem::path engineShaderRoot();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    ShaderCompilerOptions m_options;
    std::vector<std::filesystem::path> m_roots;
    std::atomic<u64> m_cacheHits{0};
    std::atomic<u64> m_cacheMisses{0};
};

// Validates a reflected shader against the engine's bindless pipeline layout
// (set 0: binding 0 sampled images, 1 storage images, 2 samplers; push constants ≤ maxPushConstants).
bool validateAgainstBindlessLayout(const ShaderReflection& r, u32 maxPushConstants, std::string& error);

u64 hashBytes(const void* data, usize size, u64 seed = 0xcbf29ce484222325ull);

} // namespace ox::rhi
