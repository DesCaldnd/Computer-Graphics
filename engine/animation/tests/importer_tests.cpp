#include "test_helpers.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#if defined(OX_HAS_SHADERC)
#include <shaderc/shaderc.hpp>
#endif

using namespace ox;
using namespace ox::anim;
using namespace ox::anim::test;

namespace {

namespace fs = std::filesystem;

// Writes a minimal skinned glTF 2.0: a 3-ring bar (12 vertices) bound to Root→Mid→Tip,
// one animation (Root translation + Mid rotation). Returns the .gltf path.
fs::path writeTestGltf(const fs::path& dir) {
    fs::create_directories(dir);
    std::vector<u8> bin;
    struct View {
        usize offset, size;
    };
    std::vector<View> views;
    auto append = [&](const void* data, usize size) {
        while (bin.size() % 4) bin.push_back(0);
        views.push_back({bin.size(), size});
        const auto* p = static_cast<const u8*>(data);
        bin.insert(bin.end(), p, p + size);
        return views.size() - 1;
    };

    std::vector<glm::vec3> positions, normals;
    std::vector<u16> joints;
    std::vector<f32> weights;
    for (int ring = 0; ring < 3; ++ring) {
        for (int k = 0; k < 4; ++k) {
            const f32 a = glm::half_pi<f32>() * static_cast<f32>(k);
            positions.push_back({0.2f * std::cos(a), static_cast<f32>(ring), 0.2f * std::sin(a)});
            normals.push_back({std::cos(a), 0.0f, std::sin(a)});
            const u16 j[3][4] = {{0, 0, 0, 0}, {0, 1, 0, 0}, {1, 0, 0, 0}};
            const f32 w[3][4] = {{1, 0, 0, 0}, {0.5f, 0.5f, 0, 0}, {1, 0, 0, 0}};
            joints.insert(joints.end(), j[ring], j[ring] + 4);
            weights.insert(weights.end(), w[ring], w[ring] + 4);
        }
    }
    std::vector<u16> indices;
    for (u16 ring = 0; ring < 2; ++ring) {
        for (u16 k = 0; k < 4; ++k) {
            const u16 a = static_cast<u16>(ring * 4 + k), b = static_cast<u16>(ring * 4 + (k + 1) % 4);
            const u16 c = static_cast<u16>(a + 4), d = static_cast<u16>(b + 4);
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    const glm::mat4 ibm[3] = {glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0, -1, 0)),
                              glm::translate(glm::mat4(1.0f), glm::vec3(0, -2, 0))};
    const f32 times[2] = {0.0f, 1.0f};
    const glm::quat q1 = glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 0, 1));
    const f32 rotations[8] = {0, 0, 0, 1, q1.x, q1.y, q1.z, q1.w}; // glTF quats are xyzw
    const f32 translations[6] = {0, 0, 0, 0, 0, 2};

    const usize vPos = append(positions.data(), positions.size() * sizeof(glm::vec3));
    const usize vNrm = append(normals.data(), normals.size() * sizeof(glm::vec3));
    const usize vJnt = append(joints.data(), joints.size() * sizeof(u16));
    const usize vWgt = append(weights.data(), weights.size() * sizeof(f32));
    const usize vIdx = append(indices.data(), indices.size() * sizeof(u16));
    const usize vIbm = append(ibm, sizeof(ibm));
    const usize vTime = append(times, sizeof(times));
    const usize vRot = append(rotations, sizeof(rotations));
    const usize vTr = append(translations, sizeof(translations));

    using json = nlohmann::json;
    json j;
    j["asset"] = {{"version", "2.0"}, {"generator", "ox_animation_tests"}};
    j["buffers"] = json::array({{{"uri", "skinned.bin"}, {"byteLength", bin.size()}}});
    j["bufferViews"] = json::array();
    for (const auto& v : views) j["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", v.offset}, {"byteLength", v.size}});
    auto acc = [](usize view, int componentType, usize count, const char* type) {
        return json{{"bufferView", view}, {"componentType", componentType}, {"count", count}, {"type", type}};
    };
    constexpr int kFloat = 5126, kU16 = 5123;
    json accessors = json::array();
    json posAcc = acc(vPos, kFloat, positions.size(), "VEC3");
    posAcc["min"] = {-0.2, 0.0, -0.2};
    posAcc["max"] = {0.2, 2.0, 0.2};
    accessors.push_back(posAcc);                                       // 0
    accessors.push_back(acc(vNrm, kFloat, normals.size(), "VEC3"));     // 1
    accessors.push_back(acc(vJnt, kU16, positions.size(), "VEC4"));     // 2
    accessors.push_back(acc(vWgt, kFloat, positions.size(), "VEC4"));   // 3
    accessors.push_back(acc(vIdx, kU16, indices.size(), "SCALAR"));     // 4
    accessors.push_back(acc(vIbm, kFloat, 3, "MAT4"));                  // 5
    json timeAcc = acc(vTime, kFloat, 2, "SCALAR");
    timeAcc["min"] = {0.0};
    timeAcc["max"] = {1.0};
    accessors.push_back(timeAcc);                                       // 6
    accessors.push_back(acc(vRot, kFloat, 2, "VEC4"));                  // 7
    accessors.push_back(acc(vTr, kFloat, 2, "VEC3"));                   // 8
    j["accessors"] = accessors;
    j["meshes"] = json::array({{{"name", "Bar"},
                                {"primitives", json::array({{{"attributes",
                                                              {{"POSITION", 0}, {"NORMAL", 1}, {"JOINTS_0", 2}, {"WEIGHTS_0", 3}}},
                                                             {"indices", 4}}})}}});
    j["skins"] = json::array({{{"joints", {1, 2, 3}}, {"inverseBindMatrices", 5}, {"skeleton", 1}}});
    j["nodes"] = json::array({
        {{"name", "BarMesh"}, {"mesh", 0}, {"skin", 0}},
        {{"name", "Root"}, {"children", {2}}},
        {{"name", "Mid"}, {"translation", {0.0, 1.0, 0.0}}, {"children", {3}}},
        {{"name", "Tip"}, {"translation", {0.0, 1.0, 0.0}}},
    });
    j["scenes"] = json::array({{{"nodes", {0, 1}}}});
    j["scene"] = 0;
    j["animations"] = json::array({{{"name", "Bend"},
                                    {"samplers", json::array({{{"input", 6}, {"output", 7}, {"interpolation", "LINEAR"}},
                                                              {{"input", 6}, {"output", 8}, {"interpolation", "LINEAR"}}})},
                                    {"channels", json::array({{{"sampler", 0}, {"target", {{"node", 2}, {"path", "rotation"}}}},
                                                              {{"sampler", 1}, {"target", {{"node", 1}, {"path", "translation"}}}}})}}});

    std::ofstream(dir / "skinned.bin", std::ios::binary).write(reinterpret_cast<const char*>(bin.data()),
                                                               static_cast<std::streamsize>(bin.size()));
    const fs::path gltf = dir / "skinned.gltf";
    std::ofstream(gltf) << j.dump(2);
    return gltf;
}

fs::path tempDir(const char* name) {
    return fs::temp_directory_path() / "ox_animation_tests" / name;
}

} // namespace

TEST(AnimImporter, GltfRoundTrip) {
    const fs::path path = writeTestGltf(tempDir("gltf"));
    std::string error;
    const auto imported = importAnimationAsset(path, {}, &error);
    ASSERT_TRUE(imported.has_value()) << error;
    const Skeleton& s = imported->skeleton;
    ASSERT_EQ(s.jointCount(), 3u);
    const i32 root = s.findJoint("Root"), mid = s.findJoint("Mid"), tip = s.findJoint("Tip");
    ASSERT_TRUE(root >= 0 && mid >= 0 && tip >= 0);
    EXPECT_EQ(s.parent(root), kNoJoint);
    EXPECT_EQ(s.parent(mid), root);
    EXPECT_EQ(s.parent(tip), mid);
    expectVec(s.bindPose()[static_cast<usize>(mid)].translation, {0, 1, 0});

    // Bind pose palette == identity (inverse binds consistent with the hierarchy).
    Pose bind;
    bind.setBind(s);
    std::vector<glm::mat4> palette;
    computeSkinningMatrices(s, bind, palette);
    for (const auto& m : palette)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) EXPECT_NEAR(m[c][r], c == r ? 1.0f : 0.0f, 1e-5f);

    ASSERT_EQ(imported->meshes.size(), 1u);
    const SkinnedMeshData& mesh = imported->meshes[0];
    EXPECT_EQ(mesh.vertexCount(), 12u);
    EXPECT_EQ(mesh.indices.size(), 48u);
    EXPECT_EQ(mesh.influencesPerVertex, 4u);
    for (usize v = 0; v < mesh.vertexCount(); ++v) {
        f32 sum = 0.0f;
        for (u32 i = 0; i < 4; ++i) sum += mesh.weights[v * 4 + i];
        EXPECT_NEAR(sum, 1.0f, 1e-5f);
    }

    ASSERT_EQ(imported->clips.size(), 1u);
    const AnimationClip& clip = imported->clips[0];
    EXPECT_NEAR(clip.duration, 1.0f, 1e-4f);
    Pose p;
    clip.sample(s, 0.5f, p);
    EXPECT_NEAR(glm::angle(p.local[static_cast<usize>(mid)].rotation), glm::quarter_pi<f32>(), 1e-3f);
    expectVec(p.local[static_cast<usize>(root)].translation, {0, 0, 1}, 1e-4f);

    // Skinned top ring at t = 1: bent 90° around Z and moved 2 along Z.
    clip.sample(s, 1.0f, p);
    computeSkinningMatrices(s, p, palette);
    std::vector<glm::vec3> skinned;
    skinMesh(mesh, palette, SkinningMethod::Linear, skinned);
    glm::vec3 top(0.0f);
    int count = 0;
    for (usize v = 0; v < mesh.vertexCount(); ++v) {
        if (std::abs(mesh.positions[v].y - 2.0f) < 1e-4f) {
            top += skinned[v];
            ++count;
        }
    }
    ASSERT_EQ(count, 4);
    expectVec(top / 4.0f, {-1.0f, 1.0f, 2.0f}, 1e-4f);

    // Imported data survives the binary stream.
    ByteWriter w;
    serialize(w, s);
    serialize(w, mesh);
    serialize(w, clip);
    const auto bytes = w.take();
    ByteReader r(bytes);
    Skeleton s2;
    SkinnedMeshData m2;
    AnimationClip c2;
    ASSERT_TRUE(deserialize(r, s2) && deserialize(r, m2) && deserialize(r, c2));
    EXPECT_EQ(m2.joints, mesh.joints);
    EXPECT_EQ(c2.tracks.size(), clip.tracks.size());
}

TEST(AnimImporter, AxisAndUnitConversion) {
    const fs::path path = writeTestGltf(tempDir("gltf_axis"));
    ImportSettings settings;
    settings.sourceUpAxis = UpAxis::Z;
    settings.unitToMeters = 0.01f;
    std::string error;
    const auto imported = importAnimationAsset(path, settings, &error);
    ASSERT_TRUE(imported.has_value()) << error;
    const Skeleton& s = imported->skeleton;
    Pose bind;
    bind.setBind(s);
    std::vector<Transform> model;
    localToModel(s, bind, model);
    // Source +Y (forward in a Z-up file) becomes engine −Z; 1 unit = 1 cm.
    expectVec(model[static_cast<usize>(s.findJoint("Tip"))].translation, {0.0f, 0.0f, -0.02f}, 1e-6f);
    std::vector<glm::mat4> palette;
    computeSkinningMatrices(s, bind, palette);
    std::vector<glm::vec3> skinned;
    skinMesh(imported->meshes[0], palette, SkinningMethod::Linear, skinned);
    for (usize v = 0; v < skinned.size(); ++v) expectVec(skinned[v], imported->meshes[0].positions[v], 1e-6f);
    // Animated root translation (0,0,2) source → (0, 0.02, 0) engine.
    Pose p;
    imported->clips[0].sample(s, 1.0f, p);
    expectVec(p.local[static_cast<usize>(s.findJoint("Root"))].translation, {0.0f, 0.02f, 0.0f}, 1e-6f);
}

TEST(AnimImporter, MissingFileFails) {
    std::string error;
    EXPECT_FALSE(importAnimationAsset("/nonexistent/file.gltf", {}, &error).has_value());
    EXPECT_FALSE(error.empty());
}

TEST(AnimShaders, SkinningComputeCompilesToSpirv) {
    const fs::path src = fs::path(OX_ANIMATION_SHADER_DIR) / "animation" / "skinning.comp";
    std::ifstream in(src);
    ASSERT_TRUE(in.good()) << src;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string code = ss.str();
#if defined(OX_HAS_SHADERC)
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    const auto result = compiler.CompileGlslToSpv(code, shaderc_compute_shader, src.string().c_str(), options);
    ASSERT_EQ(result.GetCompilationStatus(), shaderc_compilation_status_success) << result.GetErrorMessage();
    EXPECT_GT(result.cend() - result.cbegin(), 0);
#elif defined(OX_GLSLC_EXECUTABLE)
    const fs::path out = tempDir("spirv") / "skinning.spv";
    fs::create_directories(out.parent_path());
    const std::string exe = OX_GLSLC_EXECUTABLE;
    const bool isGlslang = exe.find("glslangValidator") != std::string::npos;
    std::string cmd = isGlslang
        ? std::format("\"{}\" -V --target-env vulkan1.3 \"{}\" -o \"{}\"", exe, src.string(), out.string())
        : std::format("\"{}\" --target-env=vulkan1.3 \"{}\" -o \"{}\"", exe, src.string(), out.string());
#if defined(_WIN32)
    cmd = "\"" + cmd + "\""; // cmd.exe strips the outermost quote pair of a command line that starts with a quote
#endif
    ASSERT_EQ(std::system(cmd.c_str()), 0) << cmd;
    EXPECT_GT(fs::file_size(out), 0u);
#else
    GTEST_SKIP() << "shaderc / glslc not available at configure time";
#endif
}
