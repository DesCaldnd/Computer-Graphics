#pragma once

#include <oxwald/assets/byte_stream.hpp>
#include <oxwald/core/hash.hpp>
#include <oxwald/core/result.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ox::assets::detail {

void registerImporterSettingsTypes();

inline std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

inline std::string genericPath(const std::filesystem::path& p) { return p.generic_string(); }

Result<std::vector<std::byte>> readFile(const std::filesystem::path& path);
Status writeFile(const std::filesystem::path& path, std::span<const std::byte> data);

inline std::span<const std::byte> asBytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
inline std::string asString(std::span<const std::byte> b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

// Simple chunked container used by .oxmesh:
//   char magic[4] u32 version u32 chunkCount u32 reserved
//   chunkCount × { char id[4]; u32 crc32; u64 offset; u64 size }
//   chunk payloads, each 16-byte aligned
class ChunkWriter {
public:
    void add(const char (&id)[5], std::vector<std::byte> payload) {
        Chunk c;
        std::memcpy(c.id, id, 4);
        c.data = std::move(payload);
        m_chunks.push_back(std::move(c));
    }
    template <class T>
    void addSpan(const char (&id)[5], std::span<const T> items) {
        std::vector<std::byte> b(items.size_bytes());
        if (!b.empty()) std::memcpy(b.data(), items.data(), b.size());
        add(id, std::move(b));
    }
    [[nodiscard]] std::vector<std::byte> finish(const char (&magic)[5], u32 version) const;

private:
    struct Chunk {
        char id[4];
        std::vector<std::byte> data;
    };
    std::vector<Chunk> m_chunks;
};

class ChunkReader {
public:
    static Result<ChunkReader> parse(std::span<const std::byte> data, const char (&magic)[5]);
    [[nodiscard]] u32 version() const { return m_version; }
    // Empty span when missing.
    [[nodiscard]] std::span<const std::byte> get(const char (&id)[5]) const;
    [[nodiscard]] bool has(const char (&id)[5]) const;
    template <class T>
    bool read(const char (&id)[5], std::vector<T>& out) const {
        auto s = get(id);
        if (s.size() % sizeof(T) != 0) return false;
        out.resize(s.size() / sizeof(T));
        if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
        return true;
    }

private:
    struct Chunk {
        std::array<char, 4> id;
        std::span<const std::byte> data;
    };
    u32 m_version = 0;
    std::vector<Chunk> m_chunks;
};

} // namespace ox::assets::detail
