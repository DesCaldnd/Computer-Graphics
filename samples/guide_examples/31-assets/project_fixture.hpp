// Глава 31: маленький временный проект для примеров (docs/guide/31-assets.md).
// Исходники генерируются прямо в тесте: PNG-текстуры, OBJ-модель «холм» с MTL, .oxmat, сцены.
#pragma once

#include <oxwald/assets/assets.hpp>
#include <oxwald/core/uuid.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

namespace guide31 {

namespace fs = std::filesystem;

// Временная папка проекта: <tmp>/oxwald_guide_assets_<uuid>, удаляется в деструкторе.
class TempProject {
public:
    TempProject() {
        m_root = fs::temp_directory_path() / ("oxwald_guide_assets_" + ox::Uuid::generate().toString());
        fs::create_directories(m_root / "Assets");
    }
    ~TempProject() {
        std::error_code ec;
        fs::remove_all(m_root, ec);
    }
    TempProject(const TempProject&) = delete;
    TempProject& operator=(const TempProject&) = delete;

    [[nodiscard]] const fs::path& root() const { return m_root; }
    [[nodiscard]] fs::path asset(const std::string& rel) const { return m_root / "Assets" / rel; }

private:
    fs::path m_root;
};

inline void writeText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// Цветной градиент w×h (RGBA, значения 0..1) → PNG.
inline void writeGradientPng(const fs::path& p, ox::u32 w, ox::u32 h) {
    ox::assets::Image img(w, h);
    for (ox::u32 y = 0; y < h; ++y) {
        for (ox::u32 x = 0; x < w; ++x) {
            const float u = (float(x) + 0.5f) / float(w), v = (float(y) + 0.5f) / float(h);
            img.at(x, y) = glm::vec4(u, v, 0.5f + 0.5f * std::sin(u * 6.28f), 1.0f);
        }
    }
    fs::create_directories(p.parent_path());
    ASSERT_TRUE(ox::assets::saveImagePng(p, img));
}

// Карта нормалей w×h (n * 0.5 + 0.5) → PNG.
inline void writeNormalPng(const fs::path& p, ox::u32 w, ox::u32 h) {
    ox::assets::Image img(w, h);
    for (ox::u32 y = 0; y < h; ++y) {
        for (ox::u32 x = 0; x < w; ++x) {
            const float a = float(x) / float(w) * 6.2831f, b = float(y) / float(h) * 6.2831f;
            const glm::vec3 n = glm::normalize(glm::vec3(0.4f * std::sin(a), 0.4f * std::cos(b), 1.0f));
            img.at(x, y) = glm::vec4(n * 0.5f + 0.5f, 1.0f);
        }
    }
    fs::create_directories(p.parent_path());
    ASSERT_TRUE(ox::assets::saveImagePng(p, img));
}

// «Холм»: сетка n×n вершин 8×8 м с волнистым рельефом, материал Rock с текстурой hill_albedo.png (MTL).
// Достаточно треугольников, чтобы импортёр построил LOD и несколько меш-летов.
inline void writeHillObj(const fs::path& dir, int n = 33) {
    std::string obj = "mtllib hill.mtl\no Hill\n";
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const float fx = float(x) / float(n - 1), fz = float(z) / float(n - 1);
            const float h = 0.6f * std::sin(fx * 9.0f) * std::cos(fz * 7.0f) + 0.3f * std::sin((fx + fz) * 15.0f);
            obj += "v " + std::to_string(fx * 8.0f - 4.0f) + " " + std::to_string(h) + " " + std::to_string(fz * 8.0f - 4.0f) + "\n";
            obj += "vt " + std::to_string(fx) + " " + std::to_string(fz) + "\n";
        }
    }
    obj += "usemtl Rock\n";
    auto idx = [n](int x, int z) { return std::to_string(z * n + x + 1); };
    for (int z = 0; z + 1 < n; ++z) {
        for (int x = 0; x + 1 < n; ++x) {
            const std::string a = idx(x, z), b = idx(x + 1, z), c = idx(x + 1, z + 1), d = idx(x, z + 1);
            obj += "f " + a + "/" + a + " " + d + "/" + d + " " + c + "/" + c + "\n";
            obj += "f " + a + "/" + a + " " + c + "/" + c + " " + b + "/" + b + "\n";
        }
    }
    writeText(dir / "hill.obj", obj);
    writeText(dir / "hill.mtl", "newmtl Rock\nKd 0.8 0.75 0.7\nmap_Kd hill_albedo.png\n");
    writeGradientPng(dir / "hill_albedo.png", 32, 32);
}

// Ждёт выполнения условия (опрос), не дольше timeout.
inline bool waitUntil(const std::function<bool()>& fn, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (fn()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return fn();
}

// Сдвигает mtime файла, чтобы опрашивающий FileWatcher наверняка увидел изменение.
inline void touchLater(const fs::path& p) {
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(2));
}

} // namespace guide31
