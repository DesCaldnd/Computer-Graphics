// Глава 10: импорт скелета и клипов через assimp (docs/guide/10-animation.md).
// Чтобы пример не зависел от файлов, glTF собирается в памяти (буфер — data URI base64).
#include <oxwald/animation/animation.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/constants.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace ox;
using namespace ox::anim;

namespace {

std::string base64(const std::vector<u8>& data) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (usize i = 0; i < data.size(); i += 3) {
        u32 n = u32(data[i]) << 16;
        if (i + 1 < data.size()) n |= u32(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= u32(data[i + 2]);
        out += k[(n >> 18) & 63];
        out += k[(n >> 12) & 63];
        out += i + 1 < data.size() ? k[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? k[n & 63] : '=';
    }
    return out;
}

// Два узла Root -> Mid, анимация 1 с: Root едет на 2 м по +Z (в сантиметрах — 200), Mid поворачивается на 90° вокруг Z.
std::string makeGltf() {
    const glm::quat q = glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 0, 1));
    const f32 floats[] = {
        0.0f, 1.0f,                         // times        (offset 0,  8 байт)
        0, 0, 0, 1, q.x, q.y, q.z, q.w,     // rotations    (offset 8,  32 байта, xyzw)
        0, 0, 0, 0, 0, 2,                   // translations (offset 40, 24 байта)
    };
    std::vector<u8> bin(sizeof(floats));
    std::memcpy(bin.data(), floats, sizeof(floats));
    return R"({
  "asset": {"version": "2.0"},
  "buffers": [{"byteLength": 64, "uri": "data:application/octet-stream;base64,)" + base64(bin) + R"("}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0,  "byteLength": 8},
    {"buffer": 0, "byteOffset": 8,  "byteLength": 32},
    {"buffer": 0, "byteOffset": 40, "byteLength": 24}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0.0], "max": [1.0]},
    {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC4"},
    {"bufferView": 2, "componentType": 5126, "count": 2, "type": "VEC3"}],
  "nodes": [
    {"name": "Root", "children": [1]},
    {"name": "Mid", "translation": [0.0, 1.0, 0.0]}],
  "scenes": [{"nodes": [0]}],
  "scene": 0,
  "animations": [{"name": "Bend",
    "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"},
                 {"input": 0, "output": 2, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}},
                 {"sampler": 1, "target": {"node": 0, "path": "translation"}}]}]
})";
}

} // namespace

TEST(GuideAnimationImport, FromMemoryGltf) {
    const std::string gltf = makeGltf();

    ImportSettings settings;
    settings.maxInfluences = 4; // 4 или 8 влияний на вершину
    std::string error;
    // С диска: importAnimationAsset("assets/hero.glb", settings, &error)
    std::optional<AnimationImport> imported =
        importAnimationAssetFromMemory(gltf.data(), gltf.size(), "gltf", settings, &error);
    ASSERT_TRUE(imported.has_value()) << error;

    const Skeleton& skel = imported->skeleton;
    const i32 root = skel.findJoint("Root");
    const i32 mid = skel.findJoint("Mid");
    ASSERT_GE(root, 0);
    ASSERT_GE(mid, 0);
    EXPECT_EQ(skel.parent(mid), root);
    EXPECT_TRUE(imported->meshes.empty()); // файл без меша — только анимация

    ASSERT_EQ(imported->clips.size(), 1u);
    const AnimationClip& clip = imported->clips[0];
    EXPECT_NEAR(clip.duration, 1.0f, 1e-4f);
    Pose pose;
    clip.sample(skel, 0.5f, pose);
    EXPECT_NEAR(glm::angle(pose.local[static_cast<usize>(mid)].rotation), glm::quarter_pi<f32>(), 1e-3f);
    EXPECT_NEAR(pose.local[static_cast<usize>(root)].translation.z, 1.0f, 1e-4f);
}

TEST(GuideAnimationImport, UnitConversionAndErrors) {
    const std::string gltf = makeGltf();
    ImportSettings cm;
    cm.unitToMeters = 0.01f; // исходник «в сантиметрах»
    auto imported = importAnimationAssetFromMemory(gltf.data(), gltf.size(), "gltf", cm);
    ASSERT_TRUE(imported.has_value());
    const i32 mid = imported->skeleton.findJoint("Mid");
    EXPECT_NEAR(imported->skeleton.bindPose()[static_cast<usize>(mid)].translation.y, 0.01f, 1e-6f);

    // Ошибки не бросают исключений: nullopt + текст.
    std::string error;
    EXPECT_FALSE(importAnimationAsset("/nonexistent/hero.glb", {}, &error).has_value());
    EXPECT_FALSE(error.empty());
}
