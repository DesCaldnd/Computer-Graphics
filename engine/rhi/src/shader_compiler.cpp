#include <oxwald/core/log.hpp>
#include <oxwald/rhi/environment.hpp>
#include <oxwald/rhi/shader_compiler.hpp>

#include <shaderc/shaderc.hpp>
#include <spirv-reflect/spirv_reflect.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#ifndef OX_RHI_SHADER_OPTIMIZE
#define OX_RHI_SHADER_OPTIMIZE 1
#endif
#ifndef OX_RHI_SHADER_DEBUG_INFO
#define OX_RHI_SHADER_DEBUG_INFO 1
#endif

namespace ox::rhi {

namespace fs = std::filesystem;

u64 hashBytes(const void* data, usize size, u64 seed) {
    // FNV-1a 64; fast enough for shader sources and stable across runs/platforms.
    u64 h = seed;
    const auto* p = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

namespace {

u64 hashString(std::string_view s, u64 seed = 0xcbf29ce484222325ull) { return hashBytes(s.data(), s.size(), seed); }

std::optional<std::string> readTextFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path normalized(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}

shaderc_shader_kind toShadercKind(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return shaderc_vertex_shader;
    case ShaderStage::Fragment: return shaderc_fragment_shader;
    case ShaderStage::Compute: return shaderc_compute_shader;
    case ShaderStage::Geometry: return shaderc_geometry_shader;
    case ShaderStage::TessControl: return shaderc_tess_control_shader;
    case ShaderStage::TessEval: return shaderc_tess_evaluation_shader;
    case ShaderStage::Task: return shaderc_task_shader;
    case ShaderStage::Mesh: return shaderc_mesh_shader;
    case ShaderStage::RayGen: return shaderc_raygen_shader;
    case ShaderStage::Miss: return shaderc_miss_shader;
    case ShaderStage::ClosestHit: return shaderc_closesthit_shader;
    case ShaderStage::AnyHit: return shaderc_anyhit_shader;
    case ShaderStage::Intersection: return shaderc_intersection_shader;
    case ShaderStage::Callable: return shaderc_callable_shader;
    case ShaderStage::Unknown: break;
    }
    return shaderc_glsl_infer_from_source;
}

constexpr u32 kCacheMagic = 0x5053584F; // "OXSP"
constexpr u32 kCacheVersion = 2;

struct IncludeData {
    std::string name;
    std::string content;
};

} // namespace

VkShaderStageFlagBits toVkShaderStage(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case ShaderStage::TessControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case ShaderStage::TessEval: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case ShaderStage::Task: return VK_SHADER_STAGE_TASK_BIT_EXT;
    case ShaderStage::Mesh: return VK_SHADER_STAGE_MESH_BIT_EXT;
    case ShaderStage::RayGen: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    case ShaderStage::Miss: return VK_SHADER_STAGE_MISS_BIT_KHR;
    case ShaderStage::ClosestHit: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    case ShaderStage::AnyHit: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    case ShaderStage::Intersection: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    case ShaderStage::Callable: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    case ShaderStage::Unknown: break;
    }
    return VK_SHADER_STAGE_ALL;
}

const char* shaderStageName(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return "vertex";
    case ShaderStage::Fragment: return "fragment";
    case ShaderStage::Compute: return "compute";
    case ShaderStage::Geometry: return "geometry";
    case ShaderStage::TessControl: return "tess-control";
    case ShaderStage::TessEval: return "tess-eval";
    case ShaderStage::Task: return "task";
    case ShaderStage::Mesh: return "mesh";
    case ShaderStage::RayGen: return "raygen";
    case ShaderStage::Miss: return "miss";
    case ShaderStage::ClosestHit: return "closest-hit";
    case ShaderStage::AnyHit: return "any-hit";
    case ShaderStage::Intersection: return "intersection";
    case ShaderStage::Callable: return "callable";
    case ShaderStage::Unknown: break;
    }
    return "unknown";
}

ShaderStage shaderStageFromPath(const fs::path& path) {
    fs::path p = path;
    if (p.extension() == ".glsl") {
        p = p.stem();
    }
    const std::string ext = p.extension().string();
    static const std::pair<const char*, ShaderStage> kMap[] = {
        {".vert", ShaderStage::Vertex},        {".frag", ShaderStage::Fragment},     {".comp", ShaderStage::Compute},
        {".geom", ShaderStage::Geometry},      {".tesc", ShaderStage::TessControl},  {".tese", ShaderStage::TessEval},
        {".task", ShaderStage::Task},          {".mesh", ShaderStage::Mesh},         {".rgen", ShaderStage::RayGen},
        {".rmiss", ShaderStage::Miss},         {".rchit", ShaderStage::ClosestHit},  {".rahit", ShaderStage::AnyHit},
        {".rint", ShaderStage::Intersection},  {".rcall", ShaderStage::Callable},
    };
    for (auto& [e, s] : kMap) {
        if (ext == e) {
            return s;
        }
    }
    return ShaderStage::Unknown;
}

std::optional<ShaderReflection> reflectSpirv(std::span<const u32> spirv, std::string* error) {
    SpvReflectShaderModule module{};
    if (spvReflectCreateShaderModule(spirv.size_bytes(), spirv.data(), &module) != SPV_REFLECT_RESULT_SUCCESS) {
        if (error) *error = "spirv-reflect: failed to parse module";
        return std::nullopt;
    }
    ShaderReflection r;
    if (module.entry_point_count > 0) {
        const auto& ep = module.entry_points[0];
        r.entryPoint = ep.name ? ep.name : "";
        r.localSize = {ep.local_size.x, ep.local_size.y, ep.local_size.z};
        switch (ep.shader_stage) {
        case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT: r.stage = ShaderStage::Vertex; break;
        case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT: r.stage = ShaderStage::Fragment; break;
        case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT: r.stage = ShaderStage::Compute; break;
        case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT: r.stage = ShaderStage::Geometry; break;
        case SPV_REFLECT_SHADER_STAGE_TASK_BIT_EXT: r.stage = ShaderStage::Task; break;
        case SPV_REFLECT_SHADER_STAGE_MESH_BIT_EXT: r.stage = ShaderStage::Mesh; break;
        case SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR: r.stage = ShaderStage::RayGen; break;
        case SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR: r.stage = ShaderStage::Miss; break;
        case SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: r.stage = ShaderStage::ClosestHit; break;
        case SPV_REFLECT_SHADER_STAGE_ANY_HIT_BIT_KHR: r.stage = ShaderStage::AnyHit; break;
        case SPV_REFLECT_SHADER_STAGE_INTERSECTION_BIT_KHR: r.stage = ShaderStage::Intersection; break;
        case SPV_REFLECT_SHADER_STAGE_CALLABLE_BIT_KHR: r.stage = ShaderStage::Callable; break;
        default: break;
        }
    }
    u32 count = 0;
    spvReflectEnumeratePushConstantBlocks(&module, &count, nullptr);
    std::vector<SpvReflectBlockVariable*> blocks(count);
    spvReflectEnumeratePushConstantBlocks(&module, &count, blocks.data());
    for (auto* b : blocks) {
        r.pushConstantSize = std::max(r.pushConstantSize, b->offset + b->size);
    }
    count = 0;
    spvReflectEnumerateDescriptorBindings(&module, &count, nullptr);
    std::vector<SpvReflectDescriptorBinding*> bindings(count);
    spvReflectEnumerateDescriptorBindings(&module, &count, bindings.data());
    for (auto* b : bindings) {
        ShaderReflection::Binding out;
        out.set = b->set;
        out.binding = b->binding;
        out.type = VkDescriptorType(b->descriptor_type);
        out.count = b->count;
        out.name = b->name ? b->name : "";
        r.bindings.push_back(std::move(out));
    }
    spvReflectDestroyShaderModule(&module);
    return r;
}

bool validateAgainstBindlessLayout(const ShaderReflection& r, u32 maxPushConstants, std::string& error) {
    if (r.pushConstantSize > maxPushConstants) {
        error = std::format("push constant block is {} bytes, the engine layout allows {}", r.pushConstantSize, maxPushConstants);
        return false;
    }
    for (const auto& b : r.bindings) {
        VkDescriptorType expected = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        if (b.set == 0) {
            if (b.binding == 0) expected = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            if (b.binding == 1) expected = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (b.binding == 2) expected = VK_DESCRIPTOR_TYPE_SAMPLER;
        }
        if (expected == VK_DESCRIPTOR_TYPE_MAX_ENUM || b.type != expected) {
            error = std::format("descriptor '{}' (set {}, binding {}, type {}) is not part of the bindless layout; "
                                "use bindless.glsl arrays and buffer device addresses instead",
                                b.name, b.set, b.binding, int(b.type));
            return false;
        }
    }
    return true;
}

ShaderCompilerOptions ShaderCompilerOptions::defaults() {
    ShaderCompilerOptions o;
    o.optimize = OX_RHI_SHADER_OPTIMIZE != 0;
    o.debugInfo = OX_RHI_SHADER_DEBUG_INFO != 0;
    return o;
}

fs::path ShaderCompiler::engineShaderRoot() {
    std::error_code ec;
    if (const char* env = std::getenv("OXWALD_SHADER_DIR"); env && *env) {
        return env;
    }
    fs::path packaged = executableDirectory() / "shaders";
    if (fs::exists(packaged / "common" / "bindless.glsl", ec)) {
        return packaged;
    }
#ifdef OX_RHI_ENGINE_SHADER_DIR
    return OX_RHI_ENGINE_SHADER_DIR;
#else
    return packaged;
#endif
}

struct ShaderCompiler::Impl {
    shaderc::Compiler compiler;
    std::mutex cacheMutex;
};

namespace {

class Includer final : public shaderc::CompileOptions::IncluderInterface {
public:
    Includer(const ShaderCompiler& c, std::set<fs::path>& deps, std::vector<fs::path> extraDirs)
        : m_compiler(c), m_deps(deps), m_extraDirs(std::move(extraDirs)) {}

    shaderc_include_result* GetInclude(const char* requested, shaderc_include_type type, const char* requesting,
                                       size_t) override {
        auto* data = new IncludeData;
        auto* result = new shaderc_include_result{};
        result->user_data = data;
        auto path = m_compiler.resolveInclude(requested, fs::path(requesting), type == shaderc_include_type_relative,
                                              m_extraDirs);
        std::optional<std::string> content;
        if (path) {
            content = readTextFile(*path);
        }
        if (!path || !content) {
            data->content = std::format("cannot resolve #include \"{}\" from {}", requested, requesting);
        } else {
            m_deps.insert(*path);
            data->name = path->string();
            data->content = std::move(*content);
        }
        result->source_name = data->name.c_str();
        result->source_name_length = data->name.size();
        result->content = data->content.c_str();
        result->content_length = data->content.size();
        return result;
    }

    void ReleaseInclude(shaderc_include_result* r) override {
        delete static_cast<IncludeData*>(r->user_data);
        delete r;
    }

private:
    const ShaderCompiler& m_compiler;
    std::set<fs::path>& m_deps;
    std::vector<fs::path> m_extraDirs;
};

bool writeCache(const fs::path& file, const std::vector<fs::path>& deps, const std::vector<u32>& spirv) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        auto put32 = [&](u32 v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto put64 = [&](u64 v) { f.write(reinterpret_cast<const char*>(&v), 8); };
        put32(kCacheMagic);
        put32(kCacheVersion);
        put32(u32(deps.size()));
        for (const auto& d : deps) {
            auto content = readTextFile(d);
            put64(content ? hashString(*content) : 0);
            const std::string s = d.string();
            put32(u32(s.size()));
            f.write(s.data(), std::streamsize(s.size()));
        }
        put32(u32(spirv.size()));
        f.write(reinterpret_cast<const char*>(spirv.data()), std::streamsize(spirv.size() * 4));
        if (!f) {
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

bool readCache(const fs::path& file, std::vector<fs::path>& deps, std::vector<u32>& spirv) {
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        return false;
    }
    auto get32 = [&]() { u32 v = 0; f.read(reinterpret_cast<char*>(&v), 4); return v; };
    auto get64 = [&]() { u64 v = 0; f.read(reinterpret_cast<char*>(&v), 8); return v; };
    if (get32() != kCacheMagic || get32() != kCacheVersion) {
        return false;
    }
    const u32 depCount = get32();
    if (!f || depCount > 4096) {
        return false;
    }
    for (u32 i = 0; i < depCount; ++i) {
        const u64 h = get64();
        const u32 len = get32();
        if (!f || len > 4096) {
            return false;
        }
        std::string s(len, '\0');
        f.read(s.data(), len);
        auto content = readTextFile(s);
        if (!content || hashString(*content) != h) {
            return false; // a dependency changed
        }
        deps.emplace_back(s);
    }
    const u32 words = get32();
    if (!f || words == 0 || words > (64u << 20)) {
        return false;
    }
    spirv.resize(words);
    f.read(reinterpret_cast<char*>(spirv.data()), std::streamsize(words * 4));
    return bool(f);
}

} // namespace

ShaderCompiler::ShaderCompiler(ShaderCompilerOptions options) : m_impl(std::make_unique<Impl>()), m_options(std::move(options)) {
    for (const auto& r : m_options.includeRoots) {
        m_roots.push_back(normalized(r));
    }
    if (m_options.addEngineShaderRoot) {
        m_roots.push_back(normalized(engineShaderRoot()));
    }
}

ShaderCompiler::~ShaderCompiler() = default;

std::optional<fs::path> ShaderCompiler::resolveInclude(std::string_view requested, const fs::path& requester,
                                                       bool relative, std::span<const fs::path> extraDirs) const {
    std::error_code ec;
    const fs::path req(requested);
    if (req.is_absolute()) {
        return fs::exists(req, ec) ? std::optional(normalized(req)) : std::nullopt;
    }
    if (relative && requester.has_parent_path()) {
        fs::path candidate = requester.parent_path() / req;
        if (fs::is_regular_file(candidate, ec)) {
            return normalized(candidate);
        }
    }
    for (const auto& dir : extraDirs) {
        fs::path candidate = dir / req;
        if (fs::is_regular_file(candidate, ec)) {
            return normalized(candidate);
        }
    }
    for (const auto& root : m_roots) {
        fs::path candidate = root / req;
        if (fs::is_regular_file(candidate, ec)) {
            return normalized(candidate);
        }
    }
    return std::nullopt;
}

std::optional<fs::path> ShaderCompiler::resolveSourcePath(const fs::path& path) const {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        return normalized(path);
    }
    if (path.is_relative()) {
        for (const auto& root : m_roots) {
            if (fs::is_regular_file(root / path, ec)) {
                return normalized(root / path);
            }
        }
    }
    return std::nullopt;
}

ShaderCompileResult ShaderCompiler::compile(const ShaderCompileDesc& desc) {
    ShaderCompileResult out;
    const bool inlineSource = !desc.source.empty();
    fs::path mainPath = desc.path;
    std::string source;
    if (inlineSource) {
        source = desc.source;
        if (auto resolved = resolveSourcePath(desc.path)) {
            mainPath = *resolved;
        }
    } else {
        auto resolved = resolveSourcePath(desc.path);
        if (!resolved) {
            out.errors = std::format("shader source not found: {}", desc.path.string());
            return out;
        }
        mainPath = *resolved;
        auto text = readTextFile(mainPath);
        if (!text) {
            out.errors = std::format("cannot read shader: {}", mainPath.string());
            return out;
        }
        source = std::move(*text);
    }

    const ShaderStage stage = desc.stage != ShaderStage::Unknown ? desc.stage : shaderStageFromPath(desc.path);
    if (stage == ShaderStage::Unknown) {
        out.errors = std::format("cannot infer shader stage from '{}'; set ShaderCompileDesc::stage", desc.path.string());
        return out;
    }

    // Cache key: everything that influences the output except include contents, which are validated per entry.
    std::vector<ShaderDefine> defines = m_options.globalDefines;
    defines.insert(defines.end(), desc.defines.begin(), desc.defines.end());
    u64 key = hashString(mainPath.string());
    key = hashString(desc.entryPoint, key);
    key = hashBytes(&stage, sizeof(stage), key);
    for (const auto& d : defines) {
        key = hashString(d.name, key);
        key = hashString("=", key);
        key = hashString(d.value, key);
    }
    const u32 flags = (m_options.optimize ? 1u : 0u) | (m_options.debugInfo ? 2u : 0u);
    key = hashBytes(&flags, sizeof(flags), key);
    key = hashString(source, key); // main file content is part of the key (inline sources have no file to re-hash)
    for (const auto& r : m_roots) {
        key = hashString(r.string(), key);
    }
    std::vector<fs::path> includeDirs;
    for (const auto& d : desc.includeDirs) {
        includeDirs.push_back(normalized(d));
        key = hashString("-I" + includeDirs.back().string(), key);
    }
    out.cacheKey = key;

    const bool useCache = !m_options.cacheDirectory.empty();
    const fs::path cacheFile = m_options.cacheDirectory / std::format("{:016x}.oxspv", key);
    if (useCache) {
        std::lock_guard lock(m_impl->cacheMutex);
        std::vector<fs::path> deps;
        std::vector<u32> spirv;
        if (readCache(cacheFile, deps, spirv)) {
            out.success = true;
            out.fromCache = true;
            out.spirv = std::move(spirv);
            if (!inlineSource) {
                out.dependencies.push_back(mainPath);
            }
            out.dependencies.insert(out.dependencies.end(), deps.begin(), deps.end());
            if (auto r = reflectSpirv(out.spirv, &out.errors)) {
                out.reflection = std::move(*r);
            }
            ++m_cacheHits;
            return out;
        }
    }
    if (useCache) {
        ++m_cacheMisses;
    }

    shaderc::CompileOptions opts;
    opts.SetSourceLanguage(shaderc_source_language_glsl);
    opts.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    opts.SetTargetSpirv(shaderc_spirv_version_1_5);
    opts.SetOptimizationLevel(m_options.optimize ? shaderc_optimization_level_performance : shaderc_optimization_level_zero);
    if (m_options.debugInfo) {
        opts.SetGenerateDebugInfo();
    }
    opts.SetForcedVersionProfile(460, shaderc_profile_none);
    for (const auto& d : defines) {
        opts.AddMacroDefinition(d.name, d.value);
    }
    std::set<fs::path> includes;
    opts.SetIncluder(std::make_unique<Includer>(*this, includes, includeDirs));

    const std::string inputName = mainPath.string();
    auto result = m_impl->compiler.CompileGlslToSpv(source, toShadercKind(stage), inputName.c_str(),
                                                    desc.entryPoint.c_str(), opts);
    out.errors = result.GetErrorMessage();
    if (!inlineSource) {
        out.dependencies.push_back(mainPath);
    }
    out.dependencies.insert(out.dependencies.end(), includes.begin(), includes.end());
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        if (out.errors.empty()) {
            out.errors = "shader compilation failed";
        }
        return out;
    }
    out.spirv.assign(result.cbegin(), result.cend());
    std::string reflectError;
    if (auto r = reflectSpirv(out.spirv, &reflectError)) {
        out.reflection = std::move(*r);
    } else {
        out.errors += reflectError;
        return out;
    }
    out.success = true;
    if (useCache) {
        std::lock_guard lock(m_impl->cacheMutex);
        std::vector<fs::path> deps(includes.begin(), includes.end());
        if (!writeCache(cacheFile, deps, out.spirv)) {
            OX_LOG_WARN("shader", "failed to write SPIR-V cache {}", cacheFile.string());
        }
    }
    return out;
}

} // namespace ox::rhi
