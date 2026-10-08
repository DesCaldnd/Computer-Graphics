// Глава 17: компилятор шейдеров (shaderc) — GLSL -> SPIR-V, include, define, рефлексия, проверка bindless-
// раскладки, дисковый кэш SPIR-V (docs/guide/17-rhi-vulkan.md). Только CPU, Vulkan-устройство не нужно.
#include <oxwald/rhi/pipeline.hpp>
#include <oxwald/rhi/shader_compiler.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace ox;
using namespace ox::rhi;
namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

// Compute-шейдер в стиле движка: буфер по device address, параметры в push constants.
constexpr const char* kScaleComp = R"(#version 460
#include <common/bindless.glsl>
layout(local_size_x = 64) in;
OX_BUFFER(Values, { float v[]; });
OX_PUSH_CONSTANTS({ Values values; uint count; float scale; });
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < pc.count) pc.values.v[i] *= pc.scale * SCALE_BIAS;
}
)";

} // namespace

TEST(GuideRhiShaders, CompileInlineGlslWithReflection) {
    ShaderCompilerOptions options = ShaderCompilerOptions::defaults(); // engine/shaders уже в include roots
    options.cacheDirectory.clear();                                    // без дискового кэша
    ShaderCompiler compiler(options);

    ShaderCompileDesc desc;
    desc.path = "guide_scale.comp";            // для inline-исходника — только имя в сообщениях об ошибках
    desc.source = kScaleComp;
    desc.stage = ShaderStage::Compute;         // у inline-исходника нет расширения, стадию задаём явно
    desc.defines = {{"SCALE_BIAS", "1.0"}};    // #define SCALE_BIAS 1.0

    ShaderCompileResult r = compiler.compile(desc);
    ASSERT_TRUE(r.success) << r.errors;
    EXPECT_EQ(r.spirv.front(), 0x07230203u); // магическое число SPIR-V

    // Рефлексия (spirv-reflect): размер workgroup и push constants.
    EXPECT_EQ(r.reflection.localSize[0], 64u);
    EXPECT_EQ(r.reflection.pushConstantSize, 16u); // 8 байт адрес + uint + float (scalar layout)

    // Тот же контроль, что делает Device при создании пайплайна.
    std::string error;
    EXPECT_TRUE(validateAgainstBindlessLayout(r.reflection, kMaxPushConstantSize, error)) << error;
}

TEST(GuideRhiShaders, ErrorsAndForeignDescriptors) {
    ShaderCompilerOptions options = ShaderCompilerOptions::defaults();
    options.cacheDirectory.clear();
    ShaderCompiler compiler(options);

    // Забыли define -> ошибка компиляции с именем файла.
    ShaderCompileDesc noDefine;
    noDefine.path = "guide_scale.comp";
    noDefine.source = kScaleComp;
    noDefine.stage = ShaderStage::Compute;
    ShaderCompileResult broken = compiler.compile(noDefine);
    EXPECT_FALSE(broken.success);
    EXPECT_NE(broken.errors.find("guide_scale.comp"), std::string::npos) << broken.errors;

    // Шейдер компилируется, но объявляет свой descriptor set — с bindless-раскладкой движка он несовместим.
    ShaderCompileDesc legacy;
    legacy.path = "legacy.frag";
    legacy.stage = ShaderStage::Fragment;
    legacy.source = R"(#version 460
layout(set = 1, binding = 0) uniform sampler2D legacyTexture;
layout(location = 0) out vec4 color;
void main() { color = texture(legacyTexture, vec2(0.5)); })";
    ShaderCompileResult r = compiler.compile(legacy);
    ASSERT_TRUE(r.success) << r.errors;
    std::string error;
    EXPECT_FALSE(validateAgainstBindlessLayout(r.reflection, kMaxPushConstantSize, error));
    EXPECT_NE(error.find("legacyTexture"), std::string::npos) << error;
}

TEST(GuideRhiShaders, FilesIncludesAndSpirvCache) {
    const fs::path dir = fs::temp_directory_path() / "oxwald_guide_rhi_shaders";
    fs::remove_all(dir);
    writeFile(dir / "src" / "lib" / "tint.glsl", "vec3 tint(vec3 c) { return c * TINT; }\n");
    writeFile(dir / "src" / "tint.frag", R"(#version 460
#include "lib/tint.glsl"
#include <common/bindless.glsl>
OX_PUSH_CONSTANTS({ uint tex; });
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(tint(OX_SAMPLE_2D(pc.tex, OX_SAMPLER_LINEAR_CLAMP, uv).rgb), 1.0); }
)");

    ShaderCompilerOptions options = ShaderCompilerOptions::defaults();
    options.cacheDirectory = dir / "cache";      // SPIR-V кэш на диске
    options.globalDefines = {{"TINT", "vec3(1.0, 0.9, 0.8)"}}; // define для всех шейдеров

    EXPECT_EQ(shaderStageFromPath(dir / "src" / "tint.frag"), ShaderStage::Fragment); // стадия по расширению
    {
        ShaderCompiler compiler(options);
        ShaderCompileResult first = compiler.compile({dir / "src" / "tint.frag"});
        ASSERT_TRUE(first.success) << first.errors;
        EXPECT_FALSE(first.fromCache);
        // Зависимости: сам файл + все include (по ним работает hot reload).
        EXPECT_GE(first.dependencies.size(), 3u);
    }
    {
        // Новый компилятор (как после перезапуска) берёт SPIR-V из кэша.
        ShaderCompiler compiler(options);
        ShaderCompileResult cached = compiler.compile({dir / "src" / "tint.frag"});
        ASSERT_TRUE(cached.success);
        EXPECT_TRUE(cached.fromCache);
        EXPECT_EQ(compiler.cacheHits(), 1u);

        // Правка include-файла инвалидирует запись.
        writeFile(dir / "src" / "lib" / "tint.glsl", "vec3 tint(vec3 c) { return c * TINT * 0.5; }\n");
        EXPECT_FALSE(compiler.compile({dir / "src" / "tint.frag"}).fromCache);
    }
    fs::remove_all(dir);
}
