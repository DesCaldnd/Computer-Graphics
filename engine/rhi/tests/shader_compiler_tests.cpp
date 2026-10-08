#include <oxwald/rhi/pipeline.hpp>
#include <oxwald/rhi/shader_compiler.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

using namespace ox;
using namespace ox::rhi;
namespace fs = std::filesystem;

namespace {

const fs::path kData = fs::path(OX_TEST_DATA_DIR) / "shaders";

fs::path makeTempDir(const char* tag) {
    std::random_device rd;
    fs::path dir = fs::temp_directory_path() / std::format("oxwald_rhi_{}_{:08x}", tag, rd());
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

ShaderCompilerOptions options(const fs::path& cache = {}) {
    ShaderCompilerOptions o = ShaderCompilerOptions::defaults();
    o.cacheDirectory = cache;
    return o;
}

} // namespace

TEST(ShaderCompiler, StageFromExtension) {
    EXPECT_EQ(shaderStageFromPath("a/b.vert"), ShaderStage::Vertex);
    EXPECT_EQ(shaderStageFromPath("x.frag.glsl"), ShaderStage::Fragment);
    EXPECT_EQ(shaderStageFromPath("x.comp"), ShaderStage::Compute);
    EXPECT_EQ(shaderStageFromPath("x.rchit"), ShaderStage::ClosestHit);
    EXPECT_EQ(shaderStageFromPath("x.mesh"), ShaderStage::Mesh);
    EXPECT_EQ(shaderStageFromPath("common/bindless.glsl"), ShaderStage::Unknown);
}

TEST(ShaderCompiler, ResolvesRelativeAndRootIncludes) {
    ShaderCompiler c(options());
    const fs::path main = kData / "fill.comp";
    auto rel = c.resolveInclude("sub/helper.glsl", main, true);
    ASSERT_TRUE(rel.has_value());
    EXPECT_TRUE(fs::equivalent(*rel, kData / "sub" / "helper.glsl"));
    // Nested relative include resolves against the *including* file's directory.
    auto nested = c.resolveInclude("constants.glsl", kData / "sub" / "helper.glsl", true);
    ASSERT_TRUE(nested.has_value());
    // <common/bindless.glsl> comes from the engine shader root.
    auto root = c.resolveInclude("common/bindless.glsl", main, false);
    ASSERT_TRUE(root.has_value());
    EXPECT_TRUE(fs::exists(*root));
    // Angle-bracket includes do not search the including file's directory.
    EXPECT_FALSE(c.resolveInclude("sub/helper.glsl", main, false).has_value());
    EXPECT_FALSE(c.resolveInclude("nope.glsl", main, true).has_value());
}

TEST(ShaderCompiler, CompilesWithIncludesDefinesAndReflection) {
    ShaderCompiler c(options());
    ShaderCompileDesc d;
    d.path = kData / "fill.comp";
    d.defines = {{"OFFSET_VALUE", "7u"}};
    ShaderCompileResult r = c.compile(d);
    ASSERT_TRUE(r.success) << r.errors;
    EXPECT_FALSE(r.spirv.empty());
    EXPECT_EQ(r.spirv[0], 0x07230203u) << "SPIR-V magic";
    EXPECT_EQ(r.reflection.stage, ShaderStage::Compute);
    EXPECT_EQ(r.reflection.localSize[0], 64u);
    EXPECT_EQ(r.reflection.pushConstantSize, 12u) << "8-byte buffer reference + uint, scalar layout";
    // main + helper + constants + bindless
    EXPECT_GE(r.dependencies.size(), 4u);
    std::string err;
    EXPECT_TRUE(validateAgainstBindlessLayout(r.reflection, kMaxPushConstantSize, err)) << err;
    EXPECT_FALSE(validateAgainstBindlessLayout(r.reflection, 8, err));
}

TEST(ShaderCompiler, ReportsErrorsWithFileNames) {
    ShaderCompiler c(options());
    ShaderCompileResult r = c.compile({kData / "broken.comp"});
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.errors.find("broken.comp"), std::string::npos) << r.errors;

    ShaderCompileResult m = c.compile({kData / "missing_include.frag"});
    EXPECT_FALSE(m.success);
    EXPECT_NE(m.errors.find("does/not/exist.glsl"), std::string::npos) << m.errors;

    EXPECT_FALSE(c.compile({"no_such_file.comp"}).success);
}

TEST(ShaderCompiler, InlineSourceAndValidation) {
    ShaderCompiler c(options());
    ShaderCompileDesc d;
    d.path = "inline_test";
    d.stage = ShaderStage::Fragment;
    d.source = R"(#version 460
layout(set = 1, binding = 0) uniform sampler2D legacyTexture;
layout(location = 0) out vec4 color;
void main() { color = texture(legacyTexture, vec2(0.5)); })";
    ShaderCompileResult r = c.compile(d);
    ASSERT_TRUE(r.success) << r.errors;
    std::string err;
    EXPECT_FALSE(validateAgainstBindlessLayout(r.reflection, kMaxPushConstantSize, err));
    EXPECT_NE(err.find("legacyTexture"), std::string::npos) << err;
}

TEST(ShaderCompiler, SpirvCacheHitsAndInvalidatesOnIncludeChange) {
    const fs::path dir = makeTempDir("cache");
    const fs::path src = dir / "src";
    writeFile(src / "inc.glsl", "#define VALUE 1u\n");
    writeFile(src / "main.comp", R"(#version 460
#include "inc.glsl"
#extension GL_EXT_buffer_reference : require
layout(local_size_x = 1) in;
layout(buffer_reference) buffer Out { uint x; };
layout(push_constant) uniform PC { Out o; uint v; } pc;
void main() { pc.o.x = pc.v * VALUE; }
)");
    {
        ShaderCompiler c(options(dir / "cache"));
        ShaderCompileResult a = c.compile({src / "main.comp"});
        ASSERT_TRUE(a.success) << a.errors;
        EXPECT_FALSE(a.fromCache);
        EXPECT_EQ(c.cacheMisses(), 1u);
    }
    {
        // A fresh compiler (new process) hits the disk cache.
        ShaderCompiler c(options(dir / "cache"));
        ShaderCompileResult b = c.compile({src / "main.comp"});
        ASSERT_TRUE(b.success);
        EXPECT_TRUE(b.fromCache);
        EXPECT_EQ(c.cacheHits(), 1u);
        EXPECT_GE(b.dependencies.size(), 2u) << "cached entries still report includes (hot reload needs them)";
        EXPECT_EQ(b.reflection.pushConstantSize, 12u);

        // Different defines -> different cache key.
        ShaderCompileDesc withDefine{src / "main.comp"};
        withDefine.defines = {{"EXTRA", "1"}};
        EXPECT_FALSE(c.compile(withDefine).fromCache);

        // Changing an include invalidates the entry.
        writeFile(src / "inc.glsl", "#define VALUE 2u\n");
        ShaderCompileResult d = c.compile({src / "main.comp"});
        ASSERT_TRUE(d.success);
        EXPECT_FALSE(d.fromCache);
        EXPECT_TRUE(c.compile({src / "main.comp"}).fromCache);
    }
    fs::remove_all(dir);
}

// Every stage shader shipped in engine/shaders must compile (include-only .glsl files are skipped).
TEST(ShaderCompiler, CompilesEveryEngineShader) {
    ShaderCompiler c(options());
    const fs::path root = ShaderCompiler::engineShaderRoot();
    ASSERT_TRUE(fs::exists(root / "common" / "bindless.glsl")) << root;
    u32 compiled = 0;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || shaderStageFromPath(e.path()) == ShaderStage::Unknown) continue;
        ShaderCompileResult r = c.compile({e.path()});
        EXPECT_TRUE(r.success) << e.path() << ":\n" << r.errors;
        ++compiled;
    }
    // bindless.glsl itself must at least be includable from every stage kind.
    for (ShaderStage stage : {ShaderStage::Vertex, ShaderStage::Fragment, ShaderStage::Compute}) {
        ShaderCompileDesc d;
        d.path = std::format("bindless_probe_{}", shaderStageName(stage));
        d.stage = stage;
        d.source = "#version 460\n#include <common/bindless.glsl>\n"
                   "OX_PUSH_CONSTANTS({ uint tex; uint smp; });\n"
                   "void main() {}\n";
        if (stage == ShaderStage::Compute) d.source = "#version 460\n#include <common/bindless.glsl>\nlayout(local_size_x=1) in;\nvoid main() {}\n";
        ShaderCompileResult r = c.compile(d);
        EXPECT_TRUE(r.success) << shaderStageName(stage) << ": " << r.errors;
    }
    (void)compiled;
}
