#include <oxwald/assets/importer.hpp>
#include <oxwald/core/log.hpp>

#include "internal.hpp"

namespace ox::assets {

ImportContext::ImportContext(std::filesystem::path source, std::string assetPath, AssetMeta meta, Callbacks callbacks)
    : m_source(std::move(source)), m_assetPath(std::move(assetPath)), m_meta(std::move(meta)),
      m_callbacks(std::move(callbacks)) {}

Result<std::vector<std::byte>> ImportContext::readSource() const { return detail::readFile(m_source); }

ImportedArtifact& ImportContext::setMain(AssetType type, std::vector<std::byte> data) {
    for (auto& a : m_artifacts) {
        if (a.name.empty()) {
            a.type = type;
            a.data = std::move(data);
            return a;
        }
    }
    ImportedArtifact a;
    a.uuid = m_meta.uuid;
    a.type = type;
    a.data = std::move(data);
    m_artifacts.insert(m_artifacts.begin(), std::move(a));
    return m_artifacts.front();
}

Uuid ImportContext::subAssetUuid(const Uuid& main, std::string_view name) {
    return Uuid::fromName(main.toString() + "/" + std::string(name));
}

ImportedArtifact& ImportContext::addSubAsset(std::string name, AssetType type, std::vector<std::byte> data) {
    ImportedArtifact a;
    a.uuid = subAssetUuid(m_meta.uuid, name);
    a.type = type;
    a.name = std::move(name);
    a.data = std::move(data);
    m_artifacts.push_back(std::move(a));
    return m_artifacts.back();
}

void ImportContext::addSourceDependency(const std::filesystem::path& file) {
    if (std::find(m_sourceDeps.begin(), m_sourceDeps.end(), file) == m_sourceDeps.end()) m_sourceDeps.push_back(file);
}

std::optional<Uuid> ImportContext::resolveAsset(const std::filesystem::path& file,
                                                const nlohmann::ordered_json& settingsHint) {
    addSourceDependency(file);
    if (!m_callbacks.resolve) return std::nullopt;
    return m_callbacks.resolve(file, settingsHint);
}

void ImportContext::warn(std::string message) {
    OX_LOG_WARN("assets", "{}: {}", m_assetPath, message);
    m_warnings.push_back(std::move(message));
}

void ImporterRegistry::add(std::unique_ptr<IAssetImporter> importer) { m_importers.push_back(std::move(importer)); }

IAssetImporter* ImporterRegistry::find(std::string_view name) const {
    for (auto it = m_importers.rbegin(); it != m_importers.rend(); ++it) {
        if ((*it)->name() == name) return it->get();
    }
    return nullptr;
}

IAssetImporter* ImporterRegistry::findForPath(const std::filesystem::path& path) const {
    const std::string name = detail::toLower(path.filename().string());
    IAssetImporter* best = nullptr;
    usize bestLen = 0;
    for (auto it = m_importers.rbegin(); it != m_importers.rend(); ++it) {
        for (const auto& ext : (*it)->extensions()) {
            if (ext.size() > bestLen && name.size() > ext.size() && name.ends_with(detail::toLower(ext))) {
                best = it->get();
                bestLen = ext.size();
            }
        }
    }
    return best;
}

Result<std::vector<ImportedArtifact>> importStandalone(const ImporterRegistry& importers,
                                                       const std::filesystem::path& source,
                                                       const nlohmann::ordered_json& settings) {
    IAssetImporter* importer = importers.findForPath(source);
    if (!importer) return makeError("no importer for {}", source.string());
    AssetMeta meta;
    meta.uuid = Uuid::fromName(std::filesystem::absolute(source).generic_string());
    meta.importer = std::string(importer->name());
    meta.importerVersion = importer->version();
    meta.settings = importer->defaultSettingsFor(source);
    if (settings.is_object()) meta.settings.merge_patch(settings);
    ImportContext::Callbacks callbacks;
    callbacks.resolve = [](const std::filesystem::path& p, const nlohmann::ordered_json&) -> std::optional<Uuid> {
        return Uuid::fromName(std::filesystem::absolute(p).lexically_normal().generic_string());
    };
    ImportContext ctx(source, source.filename().generic_string(), meta, callbacks);
    if (auto st = importer->import(ctx); !st) return st.error();
    return std::move(ctx.artifacts());
}

} // namespace ox::assets
