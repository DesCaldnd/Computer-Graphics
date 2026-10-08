#include <oxwald/assets/cook.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>

#include "internal.hpp"

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
        // Textures stay uncompressed so mips can be range-read (streaming); BC data barely compresses anyway.
        const bool compress = options.compress && rec->type != AssetType::Texture;
        writer.add(e.artifact, *bytes, compress ? PakCompression::Zstd : PakCompression::None, id);
        report.items.push_back({id, rec->type, rec->path, bytes->size(), 0});
        catalog.push_back(std::move(e));
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
