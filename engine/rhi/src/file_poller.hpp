#pragma once

#include <oxwald/core/types.hpp>

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace ox::rhi::detail {

// Minimal mtime/size poller for shader hot reload. Kept private to rhi on purpose: core's file watcher may
// replace it later without touching the public API.
class FilePoller {
public:
    void watch(const std::filesystem::path& path) {
        auto key = path.lexically_normal().string();
        if (!m_files.contains(key)) {
            m_files.emplace(key, stamp(path));
        }
    }
    // Returns files whose mtime or size changed since the last poll (or since watch()).
    std::vector<std::filesystem::path> poll() {
        std::vector<std::filesystem::path> changed;
        for (auto& [path, st] : m_files) {
            const Stamp now = stamp(path);
            if (now.time != st.time || now.size != st.size) {
                st = now;
                changed.emplace_back(path);
            }
        }
        return changed;
    }
    [[nodiscard]] usize size() const { return m_files.size(); }

private:
    struct Stamp {
        std::filesystem::file_time_type time{};
        u64 size = ~0ull;
    };
    static Stamp stamp(const std::filesystem::path& p) {
        std::error_code ec;
        Stamp s;
        s.time = std::filesystem::last_write_time(p, ec);
        if (ec) return {};
        s.size = std::filesystem::file_size(p, ec);
        if (ec) s.size = ~0ull;
        return s;
    }
    std::unordered_map<std::string, Stamp> m_files;
};

} // namespace ox::rhi::detail
