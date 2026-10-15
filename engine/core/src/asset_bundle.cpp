#include <oxwald/core/asset_bundle.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace ox {

namespace fs = std::filesystem;

AssetIndex::AssetIndex(std::vector<std::string> files) {
    m_files.reserve(files.size());
    for (const std::string& f : files) {
        auto n = Vfs::normalizePath(f);
        if (n && !n->empty()) m_files.push_back(std::move(*n));
    }
    std::sort(m_files.begin(), m_files.end());
    m_files.erase(std::unique(m_files.begin(), m_files.end()), m_files.end());
}

AssetIndex AssetIndex::parse(std::string_view text) {
    std::vector<std::string> files;
    usize pos = 0;
    while (pos < text.size()) {
        usize end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (!line.empty() && line.front() != '#') files.emplace_back(line);
        pos = end + 1;
    }
    return AssetIndex(std::move(files));
}

std::string AssetIndex::serialize() const {
    std::string out;
    for (const std::string& f : m_files) {
        out += f;
        out += '\n';
    }
    return out;
}

bool AssetIndex::contains(std::string_view path) const {
    return std::binary_search(m_files.begin(), m_files.end(), path, std::less<>{});
}

bool AssetIndex::containsDirectory(std::string_view dir) const {
    if (dir.empty()) return !m_files.empty();
    const std::string prefix = std::string(dir) + "/";
    auto it = std::lower_bound(m_files.begin(), m_files.end(), prefix);
    return it != m_files.end() && it->starts_with(prefix);
}

std::vector<std::string> AssetIndex::filesUnder(std::string_view dir, bool recursive) const {
    std::vector<std::string> out;
    const std::string prefix = dir.empty() ? std::string() : std::string(dir) + "/";
    for (auto it = std::lower_bound(m_files.begin(), m_files.end(), prefix); it != m_files.end(); ++it) {
        if (!it->starts_with(prefix)) break;
        if (!recursive && it->find('/', prefix.size()) != std::string::npos) continue;
        out.push_back(*it);
    }
    return out;
}

std::optional<std::string> bundlePath(std::string_view prefix, std::string_view relPath) {
    auto rel = Vfs::normalizePath(relPath);
    if (!rel) return std::nullopt;
    auto pre = Vfs::normalizePath(prefix);
    if (!pre) return std::nullopt;
    if (pre->empty()) return rel;
    if (rel->empty()) return pre;
    return *pre + "/" + *rel;
}

BundleMount::BundleMount(std::shared_ptr<const IAssetBundle> bundle, std::string prefix, std::optional<AssetIndex> index)
    : m_bundle(std::move(bundle)), m_prefix(Vfs::normalizePath(prefix).value_or(std::string())), m_index(std::move(index)) {}

bool BundleMount::exists(std::string_view relPath) const {
    auto p = bundlePath(m_prefix, relPath);
    if (!p || p->empty()) return false;
    if (m_index) return m_index->contains(*p);
    return m_bundle && m_bundle->read(*p).has_value();
}

Result<std::vector<std::byte>> BundleMount::read(std::string_view relPath) const {
    auto p = bundlePath(m_prefix, relPath);
    if (!p || p->empty()) return makeError("bundle: invalid path '{}'", relPath);
    if (m_index && !m_index->contains(*p)) return makeError("bundle: '{}' not found", *p);
    if (!m_bundle) return makeError("bundle: no backend");
    auto data = m_bundle->read(*p);
    if (!data) return makeError("bundle: cannot read '{}'", *p);
    return std::move(*data);
}

std::vector<std::string> BundleMount::list(std::string_view relDir, bool recursive) const {
    auto dir = bundlePath(m_prefix, relDir);
    if (!dir) return {};
    const usize strip = m_prefix.empty() ? 0 : m_prefix.size() + 1;
    std::vector<std::string> out;
    if (m_index) {
        for (const std::string& f : m_index->filesUnder(*dir, recursive)) out.push_back(f.substr(strip));
        return out;
    }
    if (!m_bundle) return out;
    // Without an index only the files directly inside the directory are visible.
    for (const std::string& name : m_bundle->listFiles(*dir)) {
        const std::string full = dir->empty() ? name : *dir + "/" + name;
        out.push_back(full.substr(strip));
    }
    std::sort(out.begin(), out.end());
    return out;
}

Result<BundleExtractStats> extractBundle(const IAssetBundle& bundle, const AssetIndex& index, std::string_view prefix,
                                         const fs::path& destination, bool overwrite) {
    auto pre = Vfs::normalizePath(prefix);
    if (!pre) return makeError("extractBundle: invalid prefix '{}'", prefix);
    BundleExtractStats stats;
    const usize strip = pre->empty() ? 0 : pre->size() + 1;
    for (const std::string& file : index.filesUnder(*pre, true)) {
        const fs::path target = destination / fs::path(file.substr(strip));
        auto data = bundle.read(file);
        if (!data) return makeError("extractBundle: cannot read '{}'", file);
        std::error_code ec;
        if (!overwrite && fs::exists(target, ec) && fs::file_size(target, ec) == data->size()) {
            ++stats.skipped;
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return makeError("extractBundle: cannot write '{}'", target.string());
        out.write(reinterpret_cast<const char*>(data->data()), std::streamsize(data->size()));
        if (!out) return makeError("extractBundle: write failed for '{}'", target.string());
        ++stats.files;
        stats.bytes += data->size();
    }
    return stats;
}

} // namespace ox
