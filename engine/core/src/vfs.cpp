#include <oxwald/core/vfs.hpp>

#include <oxwald/core/log.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace ox {

namespace {

bool validScheme(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
    });
}

// Generic-format string relative to `base`, or empty if p is not below base.
std::string relativeGeneric(const fs::path& p, const fs::path& base) {
    return p.lexically_relative(base).generic_string();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Vfs static helpers

std::optional<std::string> Vfs::normalizePath(std::string_view path) {
    std::vector<std::string_view> segments;
    usize pos = 0;
    while (pos <= path.size()) {
        usize end = path.find_first_of("/\\", pos);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view seg = path.substr(pos, end - pos);
        pos = end + 1;
        if (seg.empty() || seg == ".") {
            continue;
        }
        if (seg.find(':') != std::string_view::npos || seg.find('\0') != std::string_view::npos) {
            return std::nullopt; // drive letters / alternate streams / embedded NUL
        }
        if (seg == "..") {
            if (segments.empty()) {
                return std::nullopt; // escapes the mount root
            }
            segments.pop_back();
            continue;
        }
        segments.push_back(seg);
    }
    std::string out;
    for (usize i = 0; i < segments.size(); ++i) {
        if (i != 0) {
            out += '/';
        }
        out += segments[i];
    }
    return out;
}

std::optional<VfsUri> Vfs::parse(std::string_view uri) {
    const usize sep = uri.find("://");
    if (sep == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view scheme = uri.substr(0, sep);
    if (!validScheme(scheme)) {
        return std::nullopt;
    }
    auto path = normalizePath(uri.substr(sep + 3));
    if (!path) {
        return std::nullopt;
    }
    return VfsUri{std::string(scheme), std::move(*path)};
}

std::string Vfs::makeUri(std::string_view scheme, std::string_view path) {
    std::string out;
    out.reserve(scheme.size() + 3 + path.size());
    out += scheme;
    out += "://";
    out += path;
    return out;
}

// ---------------------------------------------------------------------------------------------
// Vfs

void Vfs::mount(std::string scheme, std::unique_ptr<IMountSource> source, i32 priority) {
    OX_ASSERT(validScheme(scheme), "Vfs: invalid scheme '{}'", scheme);
    OX_ASSERT(source != nullptr, "Vfs: null mount source for '{}'", scheme);
    std::unique_lock lock(m_mutex);
    auto& mounts = m_mounts[std::move(scheme)];
    mounts.push_back(Mount{std::move(source), priority, ++m_mountCounter});
    std::stable_sort(mounts.begin(), mounts.end(), [](const Mount& a, const Mount& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.order > b.order;
    });
}

void Vfs::unmount(std::string_view scheme) {
    std::unique_lock lock(m_mutex);
    m_mounts.erase(std::string(scheme));
}

bool Vfs::unmount(std::string_view scheme, const IMountSource* source) {
    std::unique_lock lock(m_mutex);
    auto it = m_mounts.find(std::string(scheme));
    if (it == m_mounts.end()) {
        return false;
    }
    const usize removed = std::erase_if(it->second, [source](const Mount& m) { return m.source.get() == source; });
    if (it->second.empty()) {
        m_mounts.erase(it);
    }
    return removed != 0;
}

bool Vfs::isMounted(std::string_view scheme) const {
    std::shared_lock lock(m_mutex);
    return mountsFor(scheme) != nullptr;
}

const std::vector<Vfs::Mount>* Vfs::mountsFor(std::string_view scheme) const {
    auto it = m_mounts.find(std::string(scheme));
    return it == m_mounts.end() || it->second.empty() ? nullptr : &it->second;
}

bool Vfs::exists(std::string_view uri) const {
    const auto parsed = parse(uri);
    if (!parsed) {
        return false;
    }
    std::shared_lock lock(m_mutex);
    const auto* mounts = mountsFor(parsed->scheme);
    if (!mounts) {
        return false;
    }
    return std::any_of(mounts->begin(), mounts->end(), [&](const Mount& m) { return m.source->exists(parsed->path); });
}

Result<std::vector<std::byte>> Vfs::readBytes(std::string_view uri) const {
    const auto parsed = parse(uri);
    if (!parsed) {
        return makeError("Vfs: invalid or unsafe URI '{}'", uri);
    }
    std::shared_lock lock(m_mutex);
    const auto* mounts = mountsFor(parsed->scheme);
    if (!mounts) {
        return makeError("Vfs: scheme '{}' is not mounted ('{}')", parsed->scheme, uri);
    }
    for (const Mount& m : *mounts) {
        if (m.source->exists(parsed->path)) {
            return m.source->read(parsed->path);
        }
    }
    return makeError("Vfs: file not found '{}'", uri);
}

Result<std::string> Vfs::readText(std::string_view uri) const {
    auto bytes = readBytes(uri);
    if (!bytes) {
        return bytes.error();
    }
    std::string text(bytes->size(), '\0');
    if (!bytes->empty()) {
        std::memcpy(text.data(), bytes->data(), bytes->size());
    }
    return text;
}

bool Vfs::writeBytes(std::string_view uri, std::span<const std::byte> data) {
    const auto parsed = parse(uri);
    if (!parsed || parsed->path.empty()) {
        OX_LOG_ERROR("vfs", "write: invalid or unsafe URI '{}'", uri);
        return false;
    }
    std::shared_lock lock(m_mutex);
    const auto* mounts = mountsFor(parsed->scheme);
    if (mounts) {
        for (const Mount& m : *mounts) {
            if (m.source->isWritable()) {
                return m.source->write(parsed->path, data);
            }
        }
    }
    OX_LOG_ERROR("vfs", "write: no writable source for '{}'", uri);
    return false;
}

bool Vfs::writeText(std::string_view uri, std::string_view text) {
    return writeBytes(uri, std::as_bytes(std::span<const char>(text.data(), text.size())));
}

std::optional<fs::path> Vfs::resolveNative(std::string_view uri) const {
    const auto parsed = parse(uri);
    if (!parsed) {
        return std::nullopt;
    }
    std::shared_lock lock(m_mutex);
    const auto* mounts = mountsFor(parsed->scheme);
    if (!mounts) {
        return std::nullopt;
    }
    for (const Mount& m : *mounts) {
        if (m.source->exists(parsed->path)) {
            if (auto p = m.source->nativePath(parsed->path)) {
                return p;
            }
        }
    }
    for (const Mount& m : *mounts) {
        if (m.source->isWritable()) {
            if (auto p = m.source->nativePath(parsed->path)) {
                return p;
            }
        }
    }
    return std::nullopt;
}

std::optional<u64> Vfs::modificationTime(std::string_view uri) const {
    const auto parsed = parse(uri);
    if (!parsed) {
        return std::nullopt;
    }
    std::shared_lock lock(m_mutex);
    const auto* mounts = mountsFor(parsed->scheme);
    if (!mounts) {
        return std::nullopt;
    }
    for (const Mount& m : *mounts) {
        if (m.source->exists(parsed->path)) {
            return m.source->modificationTime(parsed->path);
        }
    }
    return std::nullopt;
}

std::vector<std::string> Vfs::list(std::string_view uriDir, bool recursive) const {
    const auto parsed = parse(uriDir);
    if (!parsed) {
        return {};
    }
    std::set<std::string> unique;
    {
        std::shared_lock lock(m_mutex);
        const auto* mounts = mountsFor(parsed->scheme);
        if (!mounts) {
            return {};
        }
        for (const Mount& m : *mounts) {
            for (std::string& rel : m.source->list(parsed->path, recursive)) {
                unique.insert(std::move(rel));
            }
        }
    }
    std::vector<std::string> out;
    out.reserve(unique.size());
    for (const std::string& rel : unique) {
        out.push_back(makeUri(parsed->scheme, rel));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// DirectoryMount

DirectoryMount::DirectoryMount(fs::path root, bool writable) : m_writable(writable) {
    std::error_code ec;
    fs::path abs = fs::absolute(root, ec);
    m_root = (ec ? root : abs).lexically_normal();
}

std::optional<fs::path> DirectoryMount::resolve(std::string_view relPath) const {
    // Defence in depth: sources may be used directly, not only through Vfs.
    auto norm = Vfs::normalizePath(relPath);
    if (!norm) {
        return std::nullopt;
    }
    return norm->empty() ? m_root : m_root / fs::path(*norm);
}

bool DirectoryMount::exists(std::string_view relPath) const {
    auto p = resolve(relPath);
    std::error_code ec;
    return p && fs::is_regular_file(*p, ec);
}

Result<std::vector<std::byte>> DirectoryMount::read(std::string_view relPath) const {
    auto p = resolve(relPath);
    if (!p) {
        return makeError("DirectoryMount: unsafe path '{}'", relPath);
    }
    std::ifstream in(*p, std::ios::binary | std::ios::ate);
    if (!in) {
        return makeError("DirectoryMount: cannot open '{}'", p->string());
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return makeError("DirectoryMount: cannot size '{}'", p->string());
    }
    std::vector<std::byte> data(static_cast<usize>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)) {
        return makeError("DirectoryMount: read failed '{}'", p->string());
    }
    return data;
}

bool DirectoryMount::write(std::string_view relPath, std::span<const std::byte> data) {
    if (!m_writable) {
        return false;
    }
    auto p = resolve(relPath);
    if (!p || *p == m_root) {
        return false;
    }
    std::error_code ec;
    fs::create_directories(p->parent_path(), ec);
    if (ec) {
        OX_LOG_ERROR("vfs", "cannot create directory '{}': {}", p->parent_path().string(), ec.message());
        return false;
    }
    static std::atomic<u64> counter{0};
    fs::path tmp = *p;
    tmp += std::format(".tmp{}_{}", std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffff,
                       counter.fetch_add(1, std::memory_order_relaxed));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            OX_LOG_ERROR("vfs", "cannot open '{}' for writing", tmp.string());
            return false;
        }
        if (!data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        out.flush();
        if (!out) {
            OX_LOG_ERROR("vfs", "write failed '{}'", tmp.string());
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, *p, ec);
    if (ec) {
        OX_LOG_ERROR("vfs", "cannot rename '{}' -> '{}': {}", tmp.string(), p->string(), ec.message());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::vector<std::string> DirectoryMount::list(std::string_view relDir, bool recursive) const {
    std::vector<std::string> out;
    auto dir = resolve(relDir);
    std::error_code ec;
    if (!dir || !fs::is_directory(*dir, ec)) {
        return out;
    }
    const auto visit = [&](const fs::directory_entry& e) {
        std::error_code fec;
        if (e.is_regular_file(fec)) {
            out.push_back(relativeGeneric(e.path(), m_root));
        }
    };
    if (recursive) {
        for (fs::recursive_directory_iterator it(*dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            visit(*it);
        }
    } else {
        for (fs::directory_iterator it(*dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            visit(*it);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<fs::path> DirectoryMount::nativePath(std::string_view relPath) const { return resolve(relPath); }

std::optional<u64> DirectoryMount::modificationTime(std::string_view relPath) const {
    auto p = resolve(relPath);
    if (!p) {
        return std::nullopt;
    }
    std::error_code ec;
    const auto t = fs::last_write_time(*p, ec);
    if (ec) {
        return std::nullopt;
    }
    return static_cast<u64>(t.time_since_epoch().count());
}

} // namespace ox
