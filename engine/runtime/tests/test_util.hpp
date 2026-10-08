#pragma once

#include <oxwald/core/uuid.hpp>

#include <filesystem>
#include <string>

namespace ox::test {

// Unique temporary directory removed on destruction.
class TempDir {
public:
    explicit TempDir(const std::string& tag = "runtime") {
        m_path = std::filesystem::temp_directory_path() / ("oxwald_" + tag + "_" + Uuid::generate().toString());
        std::filesystem::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }
    [[nodiscard]] std::filesystem::path operator/(const std::string& s) const { return m_path / s; }

private:
    std::filesystem::path m_path;
};

} // namespace ox::test
