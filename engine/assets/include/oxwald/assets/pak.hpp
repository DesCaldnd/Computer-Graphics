#pragma once

// .oxpak archive (little-endian). Layout:
//   PakHeader (64 B) | entry data (each entry aligned to `alignment`, default 16) | TOC | string table
// TOC entries are sorted by path hash (binary search), see PakTocEntry. Entries may be zstd-compressed
// (when the build has zstd); uncompressed entries can be read by byte range (texture mip streaming) and are
// mmap-friendly thanks to the alignment. Readers memory-map the file on POSIX.
//
// Cooked games mount paks into the VFS (PakMountSource) and load assets through PakAssetSource, which reads
// the asset catalog entry "catalog.oxcat" (UUID -> type, path, dependencies, artifact entry).

#include <oxwald/assets/asset_source.hpp>
#include <oxwald/core/vfs.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox::assets {

inline constexpr char kPakMagic[4] = {'O', 'X', 'P', 'K'};
inline constexpr u32 kPakVersion = 1;
inline constexpr std::string_view kPakCatalogPath = "catalog.oxcat";

enum class PakCompression : u16 { None = 0, Zstd = 1 };

#pragma pack(push, 1)
struct PakHeader {
    char magic[4];        // "OXPK"
    u32 version;          // kPakVersion
    u32 flags;            // reserved
    u32 entryCount;
    u64 tocOffset;        // PakTocEntry[entryCount]
    u64 stringsOffset;    // UTF-8 paths, not NUL-terminated
    u64 stringsSize;
    u32 alignment;
    u32 tocCrc;           // CRC32 of TOC + string table
    u64 dataSize;         // bytes of entry data (from offset 64)
    u8 reserved[8];
};
struct PakTocEntry {
    u64 pathHash;   // fnv1a64 of the normalised path
    u64 uuidHi;     // asset UUID for artifacts, 0 for plain files
    u64 uuidLo;
    u64 offset;     // from file start
    u64 storedSize; // bytes in the file
    u64 size;       // uncompressed size
    u32 crc;        // CRC32 of the uncompressed bytes
    u16 compression;
    u16 flags;
    u32 pathOffset; // into the string table
    u32 pathSize;
};
#pragma pack(pop)
static_assert(sizeof(PakHeader) == 64);
static_assert(sizeof(PakTocEntry) == 64);

[[nodiscard]] bool pakCompressionAvailable();

class PakWriter {
public:
    explicit PakWriter(u32 alignment = 16) : m_alignment(alignment) {}

    // path: normalised relative path ("assets/<uuid>.oxtex"); data copied.
    void add(std::string path, std::span<const std::byte> data, PakCompression compression = PakCompression::None,
             Uuid uuid = {});
    [[nodiscard]] usize entryCount() const { return m_entries.size(); }
    // Atomic write (temp + rename). Compression falls back to None when it does not help or is unavailable.
    Status write(const std::filesystem::path& path, i32 zstdLevel = 6);

    struct WrittenEntry {
        std::string path;
        u64 size = 0;
        u64 storedSize = 0;
    };
    [[nodiscard]] const std::vector<WrittenEntry>& written() const { return m_written; }

private:
    struct Pending {
        std::string path;
        std::vector<std::byte> data;
        PakCompression compression;
        Uuid uuid;
    };
    u32 m_alignment;
    std::vector<Pending> m_entries;
    std::vector<WrittenEntry> m_written;
};

class PakReader {
public:
    struct Entry {
        std::string path;
        Uuid uuid;
        u64 offset = 0;
        u64 storedSize = 0;
        u64 size = 0;
        u32 crc = 0;
        PakCompression compression = PakCompression::None;
    };

    [[nodiscard]] static Result<std::shared_ptr<PakReader>> open(const std::filesystem::path& path);
    ~PakReader();
    PakReader(const PakReader&) = delete;
    PakReader& operator=(const PakReader&) = delete;

    [[nodiscard]] const Entry* find(std::string_view path) const;
    [[nodiscard]] const std::vector<Entry>& entries() const { return m_entries; }
    // Decompresses and verifies the CRC.
    [[nodiscard]] Result<std::vector<std::byte>> read(const Entry& entry) const;
    [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view path) const;
    // Byte range of the uncompressed content (cheap for uncompressed entries, no CRC check).
    [[nodiscard]] Result<std::vector<std::byte>> readRange(const Entry& entry, u64 offset, u64 size) const;
    // Zero-copy view of an uncompressed entry when the file is memory-mapped.
    [[nodiscard]] std::optional<std::span<const std::byte>> view(const Entry& entry) const;
    // Re-reads every entry and checks CRCs; returns the paths that failed.
    [[nodiscard]] std::vector<std::string> verify() const;
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }
    [[nodiscard]] u64 fileSize() const { return m_fileSize; }

private:
    PakReader() = default;
    Result<std::vector<std::byte>> readStored(const Entry& entry, u64 offset, u64 size) const;

    std::filesystem::path m_path;
    u64 m_fileSize = 0;
    std::vector<Entry> m_entries;
    std::unordered_map<std::string, usize> m_byPath;
    const std::byte* m_mapped = nullptr;
    mutable std::mutex m_fileMutex; // fallback reads without mmap
    int m_fd = -1;
};

// VFS mount of a pak (read-only): vfs.mount("game", std::make_unique<PakMountSource>(reader)).
class PakMountSource final : public IMountSource {
public:
    explicit PakMountSource(std::shared_ptr<PakReader> reader);
    [[nodiscard]] bool exists(std::string_view relPath) const override;
    [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view relPath) const override;
    [[nodiscard]] std::vector<std::string> list(std::string_view relDir, bool recursive) const override;
    [[nodiscard]] const std::shared_ptr<PakReader>& reader() const { return m_reader; }

private:
    std::shared_ptr<PakReader> m_reader;
};

// Asset catalog stored in each pak (JSON).
struct PakCatalogEntry {
    Uuid uuid;
    AssetType type = AssetType::Unknown;
    std::string path;     // source path relative to Assets/
    std::string artifact; // pak entry path
    std::vector<Uuid> dependencies;
};
[[nodiscard]] std::string catalogToJson(const std::vector<PakCatalogEntry>& entries);
[[nodiscard]] Result<std::vector<PakCatalogEntry>> catalogFromJson(std::string_view json);

// Cooked runtime source: reads only from paks (no importers). Later-added paks override earlier ones (patches).
class PakAssetSource final : public IAssetSource {
public:
    PakAssetSource() = default;
    Status addPak(std::shared_ptr<PakReader> reader);
    Status addPak(const std::filesystem::path& path);

    [[nodiscard]] std::optional<AssetRecord> record(const Uuid& uuid) override;
    [[nodiscard]] std::optional<Uuid> uuidForPath(std::string_view path) override;
    [[nodiscard]] Result<std::vector<std::byte>> readArtifact(const Uuid& uuid) override;
    [[nodiscard]] Result<std::vector<std::byte>> readArtifactRange(const Uuid& uuid, u64 offset, u64 size) override;
    [[nodiscard]] std::vector<Uuid> allAssets() override;

private:
    struct Item {
        PakCatalogEntry entry;
        std::shared_ptr<PakReader> reader;
        const PakReader::Entry* pakEntry = nullptr;
    };
    std::mutex m_mutex;
    std::vector<std::shared_ptr<PakReader>> m_paks;
    std::unordered_map<Uuid, Item> m_items;
    std::unordered_map<std::string, Uuid> m_paths;
};

} // namespace ox::assets
