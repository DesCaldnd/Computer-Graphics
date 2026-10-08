#include <oxwald/assets/pak.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>

#include "internal.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

#if OX_ASSETS_HAS_ZSTD
#include <zstd.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define OX_PAK_MMAP 1
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ox::assets {

bool pakCompressionAvailable() {
#if OX_ASSETS_HAS_ZSTD
    return true;
#else
    return false;
#endif
}

namespace {
std::string normalizeEntryPath(std::string_view path) {
    auto n = Vfs::normalizePath(path);
    return n ? *n : std::string(path);
}
} // namespace

void PakWriter::add(std::string path, std::span<const std::byte> data, PakCompression compression, Uuid uuid) {
    m_entries.push_back(Pending{normalizeEntryPath(path), std::vector<std::byte>(data.begin(), data.end()), compression, uuid});
}

Status PakWriter::write(const std::filesystem::path& path, i32 zstdLevel) {
    OX_PROFILE_ZONE();
    ByteWriter w;
    PakHeader header{};
    w.write(header); // patched at the end
    std::vector<PakTocEntry> toc;
    std::string strings;
    m_written.clear();
    std::sort(m_entries.begin(), m_entries.end(), [](const Pending& a, const Pending& b) { return a.path < b.path; });
    for (usize i = 1; i < m_entries.size(); ++i) {
        if (m_entries[i].path == m_entries[i - 1].path) return makeError("duplicate pak entry {}", m_entries[i].path);
    }
    for (auto& e : m_entries) {
        w.align(std::max(1u, m_alignment));
        PakTocEntry t{};
        t.pathHash = fnv1a64(e.path);
        t.uuidHi = e.uuid.hi;
        t.uuidLo = e.uuid.lo;
        t.offset = w.size();
        t.size = e.data.size();
        t.crc = crc32(e.data);
        t.compression = u16(PakCompression::None);
        t.pathOffset = u32(strings.size());
        t.pathSize = u32(e.path.size());
        strings += e.path;
        bool stored = false;
#if OX_ASSETS_HAS_ZSTD
        if (e.compression == PakCompression::Zstd && e.data.size() > 64) {
            std::vector<std::byte> out(ZSTD_compressBound(e.data.size()));
            const usize n = ZSTD_compress(out.data(), out.size(), e.data.data(), e.data.size(), zstdLevel);
            if (!ZSTD_isError(n) && n < e.data.size() * 15 / 16) { // keep only worthwhile compression
                w.writeBytes(out.data(), n);
                t.storedSize = n;
                t.compression = u16(PakCompression::Zstd);
                stored = true;
            }
        }
#endif
        if (!stored) {
            w.writeBytes(e.data.data(), e.data.size());
            t.storedSize = e.data.size();
        }
        m_written.push_back({e.path, t.size, t.storedSize});
        toc.push_back(t);
    }
    std::sort(toc.begin(), toc.end(), [](const PakTocEntry& a, const PakTocEntry& b) { return a.pathHash < b.pathHash; });
    w.align(16);
    std::memcpy(header.magic, kPakMagic, 4);
    header.version = kPakVersion;
    header.entryCount = u32(toc.size());
    header.alignment = m_alignment;
    header.dataSize = w.size() - sizeof(PakHeader);
    header.tocOffset = w.size();
    w.writeBytes(toc.data(), toc.size() * sizeof(PakTocEntry));
    header.stringsOffset = w.size();
    header.stringsSize = strings.size();
    w.writeBytes(strings.data(), strings.size());
    const auto& bytes = w.data();
    header.tocCrc = crc32(bytes.data() + header.tocOffset, bytes.size() - header.tocOffset);
    w.patch(0, header);
    return detail::writeFile(path, w.data());
}

Result<std::shared_ptr<PakReader>> PakReader::open(const std::filesystem::path& path) {
    OX_PROFILE_ZONE();
    std::shared_ptr<PakReader> r(new PakReader());
    r->m_path = path;
    std::vector<std::byte> tail;
    PakHeader h{};
#if defined(OX_PAK_MMAP)
    r->m_fd = ::open(path.c_str(), O_RDONLY);
    if (r->m_fd < 0) return makeError("cannot open {}", path.string());
    struct stat st {};
    if (fstat(r->m_fd, &st) != 0) return makeError("cannot stat {}", path.string());
    r->m_fileSize = u64(st.st_size);
    if (r->m_fileSize >= sizeof(PakHeader)) {
        void* p = mmap(nullptr, r->m_fileSize, PROT_READ, MAP_PRIVATE, r->m_fd, 0);
        if (p != MAP_FAILED) r->m_mapped = static_cast<const std::byte*>(p);
    }
#elif defined(_WIN32)
    // The view keeps the section alive, so the file and mapping handles are closed right away.
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return makeError("cannot open {}", path.string());
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize)) {
        CloseHandle(file);
        return makeError("cannot stat {}", path.string());
    }
    r->m_fileSize = u64(fileSize.QuadPart);
    if (r->m_fileSize >= sizeof(PakHeader)) {
        if (const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr)) {
            r->m_mapped = static_cast<const std::byte*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
            CloseHandle(mapping);
        }
    }
    CloseHandle(file);
#endif
    auto readAt = [&](u64 offset, u64 size) -> Result<std::vector<std::byte>> {
        if (offset > r->m_fileSize || size > r->m_fileSize - offset) return makeError("pak read out of range");
        if (r->m_mapped) return std::vector<std::byte>(r->m_mapped + offset, r->m_mapped + offset + size);
        std::ifstream in(path, std::ios::binary);
        in.seekg(std::streamoff(offset));
        std::vector<std::byte> out(size);
        in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
        if (u64(in.gcount()) != size) return makeError("short read");
        return out;
    };
    if (!r->m_mapped) {
        std::error_code ec;
        r->m_fileSize = std::filesystem::file_size(path, ec);
        if (ec) return makeError("cannot open {}", path.string());
    }
    auto hb = readAt(0, sizeof(PakHeader));
    if (!hb) return makeError("{}: not a pak (too small)", path.string());
    std::memcpy(&h, hb->data(), sizeof(h));
    if (std::memcmp(h.magic, kPakMagic, 4) != 0) return makeError("{}: bad pak magic", path.string());
    if (h.version > kPakVersion) return makeError("{}: pak version {} unsupported", path.string(), h.version);
    if (h.tocOffset > r->m_fileSize || h.stringsOffset + h.stringsSize > r->m_fileSize ||
        h.tocOffset + u64(h.entryCount) * sizeof(PakTocEntry) > h.stringsOffset) {
        return makeError("{}: corrupt pak header", path.string());
    }
    auto tocBytes = readAt(h.tocOffset, r->m_fileSize - h.tocOffset);
    if (!tocBytes) return tocBytes.error();
    if (crc32(*tocBytes) != h.tocCrc) return makeError("{}: pak TOC CRC mismatch", path.string());
    const auto* toc = reinterpret_cast<const PakTocEntry*>(tocBytes->data());
    const char* strings = reinterpret_cast<const char*>(tocBytes->data() + (h.stringsOffset - h.tocOffset));
    for (u32 i = 0; i < h.entryCount; ++i) {
        PakTocEntry t;
        std::memcpy(&t, toc + i, sizeof(t));
        if (u64(t.pathOffset) + t.pathSize > h.stringsSize || t.offset + t.storedSize > r->m_fileSize) {
            return makeError("{}: corrupt TOC entry {}", path.string(), i);
        }
        Entry e;
        e.path.assign(strings + t.pathOffset, t.pathSize);
        e.uuid = Uuid{t.uuidHi, t.uuidLo};
        e.offset = t.offset;
        e.storedSize = t.storedSize;
        e.size = t.size;
        e.crc = t.crc;
        e.compression = PakCompression(t.compression);
        if (fnv1a64(e.path) != t.pathHash) return makeError("{}: TOC path hash mismatch for {}", path.string(), e.path);
        r->m_byPath[e.path] = r->m_entries.size();
        r->m_entries.push_back(std::move(e));
    }
    return r;
}

PakReader::~PakReader() {
#if defined(OX_PAK_MMAP)
    if (m_mapped) munmap(const_cast<std::byte*>(m_mapped), m_fileSize);
    if (m_fd >= 0) ::close(m_fd);
#elif defined(_WIN32)
    if (m_mapped) UnmapViewOfFile(m_mapped);
#endif
}

const PakReader::Entry* PakReader::find(std::string_view path) const {
    auto it = m_byPath.find(normalizeEntryPath(path));
    return it == m_byPath.end() ? nullptr : &m_entries[it->second];
}

Result<std::vector<std::byte>> PakReader::readStored(const Entry& entry, u64 offset, u64 size) const {
    if (offset > entry.storedSize) return makeError("range outside entry");
    size = std::min(size, entry.storedSize - offset); // short read at the end, like a file
    const u64 abs = entry.offset + offset;
    if (m_mapped) return std::vector<std::byte>(m_mapped + abs, m_mapped + abs + size);
    std::lock_guard lock(m_fileMutex);
    std::ifstream in(m_path, std::ios::binary);
    in.seekg(std::streamoff(abs));
    std::vector<std::byte> out(size);
    in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
    if (u64(in.gcount()) != size) return makeError("short pak read");
    return out;
}

Result<std::vector<std::byte>> PakReader::read(const Entry& entry) const {
    auto stored = readStored(entry, 0, entry.storedSize);
    if (!stored) return stored.error();
    std::vector<std::byte> data;
    if (entry.compression == PakCompression::Zstd) {
#if OX_ASSETS_HAS_ZSTD
        data.resize(entry.size);
        const usize n = ZSTD_decompress(data.data(), data.size(), stored->data(), stored->size());
        if (ZSTD_isError(n) || n != entry.size) return makeError("zstd decompression failed for {}", entry.path);
#else
        return makeError("{} is zstd-compressed but this build has no zstd", entry.path);
#endif
    } else {
        data = std::move(*stored);
    }
    if (crc32(data) != entry.crc) return makeError("CRC mismatch in pak entry {}", entry.path);
    return data;
}

Result<std::vector<std::byte>> PakReader::read(std::string_view path) const {
    const Entry* e = find(path);
    if (!e) return makeError("{} not found in {}", path, m_path.string());
    return read(*e);
}

Result<std::vector<std::byte>> PakReader::readRange(const Entry& entry, u64 offset, u64 size) const {
    if (entry.compression == PakCompression::None) return readStored(entry, offset, size);
    auto all = read(entry);
    if (!all) return all.error();
    if (offset > all->size()) return makeError("range outside entry");
    size = std::min<u64>(size, all->size() - offset);
    return std::vector<std::byte>(all->begin() + offset, all->begin() + offset + size);
}

std::optional<std::span<const std::byte>> PakReader::view(const Entry& entry) const {
    if (!m_mapped || entry.compression != PakCompression::None) return std::nullopt;
    return std::span<const std::byte>(m_mapped + entry.offset, entry.size);
}

std::vector<std::string> PakReader::verify() const {
    std::vector<std::string> bad;
    for (const auto& e : m_entries) {
        if (!read(e)) bad.push_back(e.path);
    }
    return bad;
}

// ---- VFS mount ------------------------------------------------------------------------------------------------

PakMountSource::PakMountSource(std::shared_ptr<PakReader> reader) : m_reader(std::move(reader)) {}

bool PakMountSource::exists(std::string_view relPath) const { return m_reader->find(relPath) != nullptr; }

Result<std::vector<std::byte>> PakMountSource::read(std::string_view relPath) const { return m_reader->read(relPath); }

std::vector<std::string> PakMountSource::list(std::string_view relDir, bool recursive) const {
    std::string prefix(relDir);
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';
    std::vector<std::string> out;
    for (const auto& e : m_reader->entries()) {
        if (!e.path.starts_with(prefix)) continue;
        if (!recursive && e.path.find('/', prefix.size()) != std::string::npos) continue;
        out.push_back(e.path);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- catalog --------------------------------------------------------------------------------------------------

std::string catalogToJson(const std::vector<PakCatalogEntry>& entries) {
    nlohmann::ordered_json j;
    j["format"] = "oxcat";
    j["version"] = 1;
    auto& arr = j["assets"] = nlohmann::ordered_json::array();
    for (const auto& e : entries) {
        nlohmann::ordered_json a;
        a["uuid"] = e.uuid.toString();
        a["type"] = std::string(assetTypeName(e.type));
        a["typeId"] = u32(e.type);
        a["path"] = e.path;
        a["artifact"] = e.artifact;
        auto& d = a["dependencies"] = nlohmann::ordered_json::array();
        for (const auto& u : e.dependencies) d.push_back(u.toString());
        arr.push_back(std::move(a));
    }
    return j.dump(1);
}

Result<std::vector<PakCatalogEntry>> catalogFromJson(std::string_view json) {
    auto j = nlohmann::ordered_json::parse(json, nullptr, false);
    if (j.is_discarded() || j.value("format", std::string{}) != "oxcat") return makeError("not an asset catalog");
    std::vector<PakCatalogEntry> out;
    for (const auto& a : j.value("assets", nlohmann::ordered_json::array())) {
        PakCatalogEntry e;
        auto u = Uuid::parse(a.value("uuid", std::string{}));
        if (!u) return makeError("catalog entry without uuid");
        e.uuid = *u;
        e.type = AssetType(a.value("typeId", 0u));
        e.path = a.value("path", std::string{});
        e.artifact = a.value("artifact", std::string{});
        for (const auto& d : a.value("dependencies", nlohmann::ordered_json::array())) {
            if (auto du = Uuid::parse(d.get<std::string>())) e.dependencies.push_back(*du);
        }
        out.push_back(std::move(e));
    }
    return out;
}

// ---- cooked asset source ----------------------------------------------------------------------------------------

Status PakAssetSource::addPak(const std::filesystem::path& path) {
    auto r = PakReader::open(path);
    if (!r) return r.error();
    return addPak(std::move(*r));
}

Status PakAssetSource::addPak(std::shared_ptr<PakReader> reader) {
    auto cat = reader->read(kPakCatalogPath);
    if (!cat) return makeError("pak {} has no asset catalog: {}", reader->path().string(), cat.error().message);
    auto entries = catalogFromJson(detail::asString(*cat));
    if (!entries) return entries.error();
    std::lock_guard lock(m_mutex);
    m_paks.push_back(reader);
    for (auto& e : *entries) {
        const PakReader::Entry* pe = reader->find(e.artifact);
        if (!pe) {
            OX_LOG_WARN("assets", "catalog of {} lists missing artifact {}", reader->path().string(), e.artifact);
            continue;
        }
        if (!e.path.empty()) m_paths[e.path] = e.uuid;
        const Uuid id = e.uuid;
        m_items[id] = Item{std::move(e), reader, pe};
    }
    return {};
}

std::optional<AssetRecord> PakAssetSource::record(const Uuid& uuid) {
    std::lock_guard lock(m_mutex);
    auto it = m_items.find(uuid);
    if (it == m_items.end()) return std::nullopt;
    const auto& e = it->second.entry;
    return AssetRecord{e.uuid, e.type, e.path, e.dependencies, it->second.pakEntry->size};
}

std::optional<Uuid> PakAssetSource::uuidForPath(std::string_view path) {
    std::lock_guard lock(m_mutex);
    std::string p(path);
    if (p.starts_with("Assets/")) p = p.substr(7);
    if (auto it = m_paths.find(p); it != m_paths.end()) return it->second;
    if (auto u = Uuid::parse(p); u && m_items.count(*u)) return *u;
    return std::nullopt;
}

Result<std::vector<std::byte>> PakAssetSource::readArtifact(const Uuid& uuid) {
    std::shared_ptr<PakReader> reader;
    const PakReader::Entry* entry = nullptr;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_items.find(uuid);
        if (it == m_items.end()) return makeError("asset {} not in any pak", uuid.toString());
        reader = it->second.reader;
        entry = it->second.pakEntry;
    }
    return reader->read(*entry);
}

Result<std::vector<std::byte>> PakAssetSource::readArtifactRange(const Uuid& uuid, u64 offset, u64 size) {
    std::shared_ptr<PakReader> reader;
    const PakReader::Entry* entry = nullptr;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_items.find(uuid);
        if (it == m_items.end()) return makeError("asset {} not in any pak", uuid.toString());
        reader = it->second.reader;
        entry = it->second.pakEntry;
    }
    return reader->readRange(*entry, offset, size);
}

std::vector<Uuid> PakAssetSource::allAssets() {
    std::lock_guard lock(m_mutex);
    std::vector<Uuid> out;
    for (const auto& [id, item] : m_items) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

Result<std::vector<std::byte>> IAssetSource::readArtifactRange(const Uuid& uuid, u64 offset, u64 size) {
    auto all = readArtifact(uuid);
    if (!all) return all.error();
    if (offset > all->size()) return makeError("range outside artifact");
    size = std::min<u64>(size, all->size() - offset);
    return std::vector<std::byte>(all->begin() + offset, all->begin() + offset + size);
}

} // namespace ox::assets
