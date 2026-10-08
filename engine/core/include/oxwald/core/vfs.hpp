#pragma once

// Virtual file system: URIs of the form "<scheme>://<relative/path>" ("engine://shaders/x.glsl",
// "project://levels/a.oxscene", "user://settings.json") resolved against mount sources.
//
// Paths are normalised (backslashes -> '/', "." and empty segments removed, ".." resolved) and any
// path escaping the mount root, absolute path or drive letter is rejected. Several sources may be
// mounted per scheme; higher priority is searched first, equal priorities prefer the most recently
// mounted source (pak overlays on top of loose directories). Vfs is thread-safe (reader/writer lock
// around the mount table); mount sources must be safe for concurrent const calls.

#include <oxwald/core/result.hpp>
#include <oxwald/core/types.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ox {

class IMountSource {
public:
    virtual ~IMountSource() = default;

    // relPath / relDir arguments are already normalised (see Vfs::normalizePath); "" is the root.
    [[nodiscard]] virtual bool exists(std::string_view relPath) const = 0;
    [[nodiscard]] virtual Result<std::vector<std::byte>> read(std::string_view relPath) const = 0;
    virtual bool write(std::string_view relPath, std::span<const std::byte> data) { return false; }
    [[nodiscard]] virtual bool isWritable() const { return false; }
    // Files (not directories) below relDir, as paths relative to the mount root.
    [[nodiscard]] virtual std::vector<std::string> list(std::string_view relDir, bool recursive) const { return {}; }
    // Native file system path for loose-file sources, nullopt for archives / in-memory sources.
    [[nodiscard]] virtual std::optional<std::filesystem::path> nativePath(std::string_view relPath) const {
        return std::nullopt;
    }
    // Opaque monotonic modification stamp (e.g. file_time ticks), nullopt when unknown/missing.
    [[nodiscard]] virtual std::optional<u64> modificationTime(std::string_view relPath) const { return std::nullopt; }
};

// Loose files under a root directory. Writes are atomic (temp file + rename) and create parent dirs.
class DirectoryMount final : public IMountSource {
public:
    explicit DirectoryMount(std::filesystem::path root, bool writable = true);

    [[nodiscard]] bool exists(std::string_view relPath) const override;
    [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view relPath) const override;
    bool write(std::string_view relPath, std::span<const std::byte> data) override;
    [[nodiscard]] bool isWritable() const override { return m_writable; }
    [[nodiscard]] std::vector<std::string> list(std::string_view relDir, bool recursive) const override;
    [[nodiscard]] std::optional<std::filesystem::path> nativePath(std::string_view relPath) const override;
    [[nodiscard]] std::optional<u64> modificationTime(std::string_view relPath) const override;

    [[nodiscard]] const std::filesystem::path& root() const { return m_root; }

private:
    [[nodiscard]] std::optional<std::filesystem::path> resolve(std::string_view relPath) const;

    std::filesystem::path m_root;
    bool m_writable;
};

struct VfsUri {
    std::string scheme;
    std::string path; // normalised, relative
};

class Vfs {
public:
    Vfs() = default;
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    void mount(std::string scheme, std::unique_ptr<IMountSource> source, i32 priority = 0);
    // Removes every source of the scheme.
    void unmount(std::string_view scheme);
    // Removes one source; returns false when it was not mounted.
    bool unmount(std::string_view scheme, const IMountSource* source);
    [[nodiscard]] bool isMounted(std::string_view scheme) const;

    [[nodiscard]] bool exists(std::string_view uri) const;
    [[nodiscard]] Result<std::vector<std::byte>> readBytes(std::string_view uri) const;
    [[nodiscard]] Result<std::string> readText(std::string_view uri) const;
    // Writes to the highest-priority writable source of the scheme.
    bool writeBytes(std::string_view uri, std::span<const std::byte> data);
    bool writeText(std::string_view uri, std::string_view text);
    // Native path of the source that has the file; for missing files, the path in the first writable
    // source that has native paths (so editors can create files). nullopt otherwise.
    [[nodiscard]] std::optional<std::filesystem::path> resolveNative(std::string_view uri) const;
    [[nodiscard]] std::optional<u64> modificationTime(std::string_view uri) const;
    // Union of the files of every source (deduplicated, sorted), returned as full URIs.
    [[nodiscard]] std::vector<std::string> list(std::string_view uriDir, bool recursive = false) const;

    // "scheme://a/b" -> {scheme, normalised path}; nullopt for malformed URIs or escaping paths.
    [[nodiscard]] static std::optional<VfsUri> parse(std::string_view uri);
    // Normalises a relative path; nullopt if it is absolute, has a drive letter or escapes via "..".
    [[nodiscard]] static std::optional<std::string> normalizePath(std::string_view path);
    [[nodiscard]] static std::string makeUri(std::string_view scheme, std::string_view path);

private:
    struct Mount {
        std::unique_ptr<IMountSource> source;
        i32 priority = 0;
        u64 order = 0;
    };
    [[nodiscard]] const std::vector<Mount>* mountsFor(std::string_view scheme) const;

    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, std::vector<Mount>> m_mounts; // sorted by search order
    u64 m_mountCounter = 0;
};

} // namespace ox
