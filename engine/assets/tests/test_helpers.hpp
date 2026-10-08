#pragma once

#include <oxwald/assets/assets.hpp>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace oxtest {

namespace fs = std::filesystem;
using namespace ox;
using namespace ox::assets;

// Fresh directory per test, removed on destruction.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string name = info ? std::string(info->test_suite_name()) + "_" + info->name() : "test";
        m_path = fs::temp_directory_path() / ("oxassets_" + name + "_" + std::to_string(::getpid()) + "_" +
                                              std::to_string(counter++));
        fs::remove_all(m_path);
        fs::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    const fs::path& path() const { return m_path; }
    fs::path operator/(const std::string& s) const { return m_path / s; }

private:
    fs::path m_path;
};

inline void writeText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

inline void writeBytes(const fs::path& p, std::span<const std::byte> bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

inline std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Ensures a different mtime for change detection on file systems with coarse timestamps.
inline void touchLater(const fs::path& p) {
    const auto t = fs::last_write_time(p);
    fs::last_write_time(p, t + std::chrono::seconds(2));
}

inline Image gradientImage(u32 w, u32 h) {
    Image img(w, h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f32 u = (f32(x) + 0.5f) / f32(w), v = (f32(y) + 0.5f) / f32(h);
            img.at(x, y) = glm::vec4(u, v, 0.5f + 0.5f * std::sin(u * 6.28f) * std::cos(v * 3.14f), 1.0f);
        }
    }
    return img;
}

inline Image normalMapImage(u32 w, u32 h) {
    Image img(w, h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f32 a = (f32(x) / f32(w)) * 6.2831f, b = (f32(y) / f32(h)) * 6.2831f;
            glm::vec3 n = glm::normalize(glm::vec3(0.4f * std::sin(a), 0.4f * std::cos(b), 1.0f));
            img.at(x, y) = glm::vec4(n * 0.5f + 0.5f, 1.0f);
        }
    }
    return img;
}

inline void writePng(const fs::path& p, const Image& img) { writeBytes(p, encodePng(img)); }

// Writes "<dir>/<name>.gltf" + ".bin": a box mesh with two primitives (+Y face separate, other faces) using two
// materials, under a parent node with translation and a child node scaled x2. No tangents (generated on import).
// Material 0 references "<name>_albedo.png" and "<name>_n.png" (created too).
inline fs::path writeTestGltf(const fs::path& dir, const std::string& name) {
    struct V {
        glm::vec3 p, n;
        glm::vec2 uv;
    };
    std::vector<V> verts;
    std::vector<u16> idxTop, idxRest;
    const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        const glm::vec3 n = normals[f];
        const glm::vec3 u = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::normalize(glm::cross(glm::vec3(0, 1, 0), n));
        const glm::vec3 v = glm::cross(n, u);
        const u16 base = u16(verts.size());
        const glm::vec2 c[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (auto k : c) verts.push_back({(n + u * k.x + v * k.y) * 0.5f, n, glm::vec2(k.x * 0.5f + 0.5f, 0.5f - k.y * 0.5f)});
        auto& dst = f == 2 ? idxTop : idxRest;
        for (u16 i : {0, 1, 2, 0, 2, 3}) dst.push_back(u16(base + i));
    }
    std::vector<std::byte> bin;
    auto append = [&](const void* data, usize size) {
        const auto* b = static_cast<const std::byte*>(data);
        bin.insert(bin.end(), b, b + size);
        while (bin.size() % 4) bin.push_back(std::byte{0});
    };
    std::vector<glm::vec3> pos, nrm;
    std::vector<glm::vec2> uvs;
    for (auto& vv : verts) {
        pos.push_back(vv.p);
        nrm.push_back(vv.n);
        uvs.push_back(vv.uv);
    }
    const usize posOff = bin.size();
    append(pos.data(), pos.size() * 12);
    const usize nrmOff = bin.size();
    append(nrm.data(), nrm.size() * 12);
    const usize uvOff = bin.size();
    append(uvs.data(), uvs.size() * 8);
    const usize topOff = bin.size();
    append(idxTop.data(), idxTop.size() * 2);
    const usize restOff = bin.size();
    append(idxRest.data(), idxRest.size() * 2);
    writeBytes(dir / (name + ".bin"), bin);
    writePng(dir / (name + "_albedo.png"), gradientImage(32, 32));
    writePng(dir / (name + "_n.png"), normalMapImage(32, 32));

    const usize n = verts.size();
    std::string json = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "Root", "translation": [0, 1, 0], "children": [1]},
    {"name": "Box", "mesh": 0, "scale": [2, 2, 2]}
  ],
  "meshes": [{"name": "BoxMesh", "primitives": [
    {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0},
    {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 4, "material": 1}
  ]}],
  "materials": [
    {"name": "Painted", "pbrMetallicRoughness": {"baseColorFactor": [1, 0.5, 0.25, 1], "metallicFactor": 0.0,
      "roughnessFactor": 0.7, "baseColorTexture": {"index": 0}}, "normalTexture": {"index": 1, "scale": 0.8}},
    {"name": "Metal", "pbrMetallicRoughness": {"metallicFactor": 1.0, "roughnessFactor": 0.2}, "alphaMode": "MASK",
     "alphaCutoff": 0.3, "doubleSided": true}
  ],
  "textures": [{"source": 0}, {"source": 1}],
  "images": [{"uri": "NAME_albedo.png"}, {"uri": "NAME_n.png"}],
  "buffers": [{"uri": "NAME.bin", "byteLength": BINLEN}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": POSOFF, "byteLength": POSLEN},
    {"buffer": 0, "byteOffset": NRMOFF, "byteLength": POSLEN},
    {"buffer": 0, "byteOffset": UVOFF, "byteLength": UVLEN},
    {"buffer": 0, "byteOffset": TOPOFF, "byteLength": 12},
    {"buffer": 0, "byteOffset": RESTOFF, "byteLength": 60}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": COUNT, "type": "VEC3", "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
    {"bufferView": 1, "componentType": 5126, "count": COUNT, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": COUNT, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
    {"bufferView": 4, "componentType": 5123, "count": 30, "type": "SCALAR"}
  ]
})";
    auto replace = [&](const std::string& key, const std::string& value) {
        for (usize p = json.find(key); p != std::string::npos; p = json.find(key)) json.replace(p, key.size(), value);
    };
    replace("NAME", name);
    replace("BINLEN", std::to_string(bin.size()));
    replace("POSOFF", std::to_string(posOff));
    replace("POSLEN", std::to_string(n * 12));
    replace("NRMOFF", std::to_string(nrmOff));
    replace("UVOFF", std::to_string(uvOff));
    replace("UVLEN", std::to_string(n * 8));
    replace("TOPOFF", std::to_string(topOff));
    replace("RESTOFF", std::to_string(restOff));
    replace("COUNT", std::to_string(n));
    const fs::path path = dir / (name + ".gltf");
    writeText(path, json);
    return path;
}

inline fs::path legacyDir() { return fs::path(OX_LEGACY_ASSETS_DIR); }

// Polls `fn` until true or the timeout elapses.
template <class Fn>
bool waitUntil(Fn&& fn, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (fn()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return fn();
}

} // namespace oxtest
