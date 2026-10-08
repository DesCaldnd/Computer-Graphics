#include "internal.hpp"

#include <oxwald/core/serial/format.hpp>

#include <fstream>

namespace ox::assets::detail {

Result<std::vector<std::byte>> readFile(const std::filesystem::path& path) {
    return serial::readFileBytes(path);
}

Status writeFile(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    return serial::writeFileAtomic(path, data);
}

std::vector<std::byte> ChunkWriter::finish(const char (&magic)[5], u32 version) const {
    ByteWriter w;
    w.writeBytes(magic, 4);
    w.write(version);
    w.write(static_cast<u32>(m_chunks.size()));
    w.write(u32{0});
    const usize tableStart = w.size();
    for (usize i = 0; i < m_chunks.size(); ++i) {
        w.writeBytes(m_chunks[i].id, 4);
        w.write(u32{0});
        w.write(u64{0});
        w.write(u64{0});
    }
    for (usize i = 0; i < m_chunks.size(); ++i) {
        w.align(16);
        const usize entry = tableStart + i * 24;
        const auto& c = m_chunks[i];
        w.patch(entry + 4, crc32(c.data));
        w.patch(entry + 8, static_cast<u64>(w.size()));
        w.patch(entry + 16, static_cast<u64>(c.data.size()));
        w.writeBytes(c.data.data(), c.data.size());
    }
    return w.take();
}

Result<ChunkReader> ChunkReader::parse(std::span<const std::byte> data, const char (&magic)[5]) {
    ByteReader r(data);
    char m[4];
    r.readBytes(m, 4);
    if (r.failed() || std::memcmp(m, magic, 4) != 0) return makeError("bad magic (expected {})", magic);
    ChunkReader out;
    out.m_version = r.read<u32>();
    const u32 count = r.read<u32>();
    r.read<u32>();
    if (r.failed() || count > 4096) return makeError("corrupt chunk table");
    for (u32 i = 0; i < count; ++i) {
        Chunk c;
        r.readBytes(c.id.data(), 4);
        const u32 crc = r.read<u32>();
        const u64 offset = r.read<u64>();
        const u64 size = r.read<u64>();
        if (r.failed() || offset > data.size() || size > data.size() - offset) {
            return makeError("chunk {} out of range", i);
        }
        c.data = data.subspan(offset, size);
        if (crc32(c.data) != crc) {
            return makeError("chunk '{}' CRC mismatch", std::string_view(c.id.data(), 4));
        }
        out.m_chunks.push_back(c);
    }
    return out;
}

std::span<const std::byte> ChunkReader::get(const char (&id)[5]) const {
    for (const auto& c : m_chunks) {
        if (std::memcmp(c.id.data(), id, 4) == 0) return c.data;
    }
    return {};
}

bool ChunkReader::has(const char (&id)[5]) const {
    for (const auto& c : m_chunks) {
        if (std::memcmp(c.id.data(), id, 4) == 0) return true;
    }
    return false;
}

} // namespace ox::assets::detail
