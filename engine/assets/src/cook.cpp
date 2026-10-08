#include <oxwald/assets/cook.hpp>
#include <oxwald/core/hash.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>

#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace ox::assets {

Result<CookReport> cookProject(AssetRegistry& registry, const std::filesystem::path& outputPak, CookOptions options) {
    OX_PROFILE_ZONE();
    if (auto bytes = detail::readFile(registry.projectDir() / "pack.json")) {
        auto j = nlohmann::json::parse(detail::asString(*bytes), nullptr, false);
        if (!j.is_discarded()) {
            for (const auto& s : j.value("startupScenes", nlohmann::json::array())) options.startupScenes.push_back(s.get<std::string>());
            for (const auto& s : j.value("alwaysInclude", nlohmann::json::array())) options.alwaysInclude.push_back(s.get<std::string>());
        }
    }
    registry.scan();
    CookReport report;
    std::vector<Uuid> roots;
    auto resolve = [&](const std::string& ref) -> bool {
        if (auto u = registry.uuidForPath(ref)) {
            roots.push_back(*u);
            return true;
        }
        return false;
    };
    for (const auto& s : options.startupScenes) {
        if (!resolve(s)) report.errors.push_back("startup scene not found: " + s);
    }
    const auto all = registry.allAssets();
    for (const auto& s : options.alwaysInclude) {
        if (resolve(s)) continue;
        bool any = false;
        for (const Uuid& id : all) { // directory prefix
            auto info = registry.info(id);
            if (info && !info->parent.isValid() && info->path.starts_with(s)) {
                roots.push_back(id);
                any = true;
            }
        }
        if (!any) report.errors.push_back("always-include entry matches nothing: " + s);
    }
    if (options.includeAll || (options.startupScenes.empty() && options.alwaysInclude.empty())) {
        for (const Uuid& id : all) {
            auto info = registry.info(id);
            if (info && !info->parent.isValid()) roots.push_back(id);
            if (info) {
                for (const Uuid& s : info->subAssets) roots.push_back(s);
            }
        }
    }
    // Import (on demand) and close over dependencies; sub-assets of imported models appear after import.
    std::vector<Uuid> closure = registry.collectDependencies(roots);
    if (options.includeAll || (options.startupScenes.empty() && options.alwaysInclude.empty())) {
        std::set<Uuid> seen(closure.begin(), closure.end());
        for (const Uuid& id : registry.allAssets()) {
            if (seen.insert(id).second && registry.record(id)) closure.push_back(id);
        }
    }

    std::vector<std::shared_ptr<PakReader>> bases;
    for (const auto& basePath : options.patchBase) {
        auto base = PakReader::open(basePath);
        if (!base) return makeError("patch base '{}': {}", basePath.string(), base.error().message);
        bases.push_back(*base);
    }
    // Patch paks: identical to the base => not stored again.
    auto unchanged = [&](std::string_view path, std::span<const std::byte> bytes) {
        if (bases.empty()) return false;
        const u32 crc = crc32(bytes);
        for (const auto& b : bases) {
            if (const auto* e = b->find(path); e && e->size == bytes.size() && e->crc == crc) return true;
        }
        return false;
    };

    PakWriter writer(options.alignment);
    std::vector<PakCatalogEntry> catalog;
    for (const Uuid& id : closure) {
        auto rec = registry.record(id);
        if (!rec) {
            report.errors.push_back("cannot cook " + id.toString());
            continue;
        }
        auto bytes = registry.readArtifact(id);
        if (!bytes) {
            report.errors.push_back(rec->path + ": " + bytes.error().message);
            continue;
        }
        PakCatalogEntry e;
        e.uuid = id;
        e.type = rec->type;
        e.path = rec->path;
        e.artifact = "assets/" + id.toString() + std::string(artifactExtension(rec->type));
        e.dependencies = rec->dependencies;
        if (unchanged(e.artifact, *bytes)) {
            ++report.unchangedSkipped;
            continue;
        }
        // Textures stay uncompressed so mips can be range-read (streaming); BC data barely compresses anyway.
        const bool compress = options.compress && rec->type != AssetType::Texture;
        writer.add(e.artifact, *bytes, compress ? PakCompression::Zstd : PakCompression::None, id);
        report.items.push_back({id, rec->type, rec->path, bytes->size(), 0});
        catalog.push_back(std::move(e));
    }
    // Project files (<Name>.oxproj) travel at the pak root so a cooked game runs from the pak alone
    // (`OxwaldPlayer --pak Game.oxpak`); the runtime reads the first one when no project directory is given.
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(registry.projectDir(), ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".oxproj") continue;
            if (auto bytes = detail::readFile(entry.path())) {
                const std::string path = entry.path().filename().generic_string();
                if (unchanged(path, *bytes)) {
                    ++report.unchangedSkipped;
                    continue;
                }
                writer.add(path, *bytes, options.compress ? PakCompression::Zstd : PakCompression::None);
            }
        }
    }
    // Loose files (game UI documents, data tables, baked render data) that no importer turns into assets.
    std::vector<std::filesystem::path> looseCandidates;
    if (!options.looseFileExtensions.empty()) {
        auto lower = [](std::string v) {
            for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
            return v;
        };
        std::set<std::string> extensions;
        for (const auto& e : options.looseFileExtensions) extensions.insert(lower(e.starts_with('.') ? e : "." + e));
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (auto it = std::filesystem::recursive_directory_iterator(registry.assetsDir(), ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) && extensions.count(lower(it->path().extension().string()))) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end()); // deterministic pak layout
        for (const auto& file : files) {
            const std::string assetPath = std::filesystem::relative(file, registry.assetsDir(), ec).generic_string();
            if (registry.uuidForPath(assetPath)) continue; // imported asset (cooked above when referenced)
            looseCandidates.push_back(file);
        }
    }
    for (const std::string& dir : options.looseDirectories) {
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (auto it = std::filesystem::recursive_directory_iterator(registry.projectDir() / dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec)) files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        looseCandidates.insert(looseCandidates.end(), files.begin(), files.end());
    }
    {
        std::error_code ec;
        for (const auto& file : looseCandidates) {
            const std::string path = std::filesystem::relative(file, registry.projectDir(), ec).generic_string();
            if (ec || path.empty() || path.starts_with("..")) continue;
            auto bytes = detail::readFile(file);
            if (!bytes) {
                report.errors.push_back(path + ": " + bytes.error().message);
                continue;
            }
            if (unchanged(path, *bytes)) {
                ++report.unchangedSkipped;
                continue;
            }
            writer.add(path, *bytes, options.compress ? PakCompression::Zstd : PakCompression::None);
            report.looseFiles.push_back(path);
        }
    }
    const std::string cat = catalogToJson(catalog);
    writer.add(std::string(kPakCatalogPath), detail::asBytes(cat), options.compress ? PakCompression::Zstd : PakCompression::None);
    if (auto st = writer.write(outputPak, options.compressionLevel); !st) return st.error();
    for (auto& item : report.items) {
        const std::string artifact = "assets/" + item.uuid.toString() + std::string(artifactExtension(item.type));
        for (const auto& w : writer.written()) {
            if (w.path == artifact) item.storedSize = w.storedSize;
        }
        report.totalSize += item.size;
        report.totalStored += item.storedSize;
    }
    std::error_code ec;
    report.pakSize = std::filesystem::file_size(outputPak, ec);
    OX_LOG_INFO("assets", "cooked {} assets into {} ({} bytes)", report.items.size(), outputPak.string(), report.pakSize);
    return report;
}

} // namespace ox::assets
