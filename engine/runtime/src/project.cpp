#include <oxwald/core/log.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/runtime/json_io.hpp>
#include <oxwald/runtime/project.hpp>

#include <algorithm>

namespace ox {

bool ProjectSettings::moduleEnabled(std::string_view module) const {
    auto it = modules.find(std::string(module));
    return it == modules.end() || it->second;
}

void registerProjectTypes() {
    registerInputTypes();
    OX_REFLECT_TYPE(ProjectPhysicsSettings, "ProjectPhysicsSettings")
        .field("gravity", &ProjectPhysicsSettings::gravity)
        .field("fixedRate", &ProjectPhysicsSettings::fixedRate, attr::Range{1.0, 1000.0})
        .field("maxSubsteps", &ProjectPhysicsSettings::maxSubsteps)
        .field("maxBodies", &ProjectPhysicsSettings::maxBodies);
    OX_REFLECT_TYPE(ProjectAudioSettings, "ProjectAudioSettings")
        .field("sampleRate", &ProjectAudioSettings::sampleRate)
        .field("maxVoices", &ProjectAudioSettings::maxVoices)
        .field("busVolumes", &ProjectAudioSettings::busVolumes);
    OX_REFLECT_TYPE(ProjectRenderingSettings, "ProjectRenderingSettings")
        .field("cvars", &ProjectRenderingSettings::cvars)
        .field("rayTracingIfSupported", &ProjectRenderingSettings::rayTracingIfSupported)
        .field("upscaler", &ProjectRenderingSettings::upscaler);
    OX_REFLECT_TYPE(ProjectPackagingSettings, "ProjectPackagingSettings")
        .field("alwaysIncludeAssets", &ProjectPackagingSettings::alwaysIncludeAssets)
        .field("targetPlatforms", &ProjectPackagingSettings::targetPlatforms)
        .field("outputDir", &ProjectPackagingSettings::outputDir)
        .field("compress", &ProjectPackagingSettings::compress);
    OX_REFLECT_TYPE(ProjectSettings, "ProjectSettings")
        .field("name", &ProjectSettings::name)
        .field("version", &ProjectSettings::version)
        .field("company", &ProjectSettings::company)
        .field("startupScene", &ProjectSettings::startupScene)
        .field("assetDirs", &ProjectSettings::assetDirs)
        .field("modules", &ProjectSettings::modules)
        .field("saveVersion", &ProjectSettings::saveVersion)
        .field("input", &ProjectSettings::input)
        .field("physics", &ProjectSettings::physics)
        .field("audio", &ProjectSettings::audio)
        .field("rendering", &ProjectSettings::rendering)
        .field("defaultQuality", &ProjectSettings::defaultQuality)
        .field("scalability", &ProjectSettings::scalability)
        .field("packaging", &ProjectSettings::packaging);
}

Result<Project> Project::load(const std::filesystem::path& fileOrDirectory) {
    registerProjectTypes();
    std::error_code ec;
    std::filesystem::path file = fileOrDirectory;
    if (std::filesystem::is_directory(fileOrDirectory, ec)) {
        std::vector<std::filesystem::path> candidates;
        for (const auto& entry : std::filesystem::directory_iterator(fileOrDirectory, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == kExtension) candidates.push_back(entry.path());
        }
        if (candidates.empty()) return makeError("no {} file in '{}'", kExtension, fileOrDirectory.string());
        std::sort(candidates.begin(), candidates.end());
        if (candidates.size() > 1) {
            OX_LOG_WARN("project", "several project files in '{}', using {}", fileOrDirectory.string(),
                        candidates.front().filename().string());
        }
        file = candidates.front();
    }
    auto json = json::loadFile(file);
    if (!json) return json.error();
    Project project;
    project.m_file = std::filesystem::absolute(file, ec);
    if (!json::fromPlain(*json, project.settings)) return makeError("{}: not a project file", file.string());
    return project;
}

Project Project::create(const std::filesystem::path& directory, std::string name) {
    registerProjectTypes();
    Project p;
    p.m_file = directory / (name + std::string(kExtension));
    p.settings.name = std::move(name);
    return p;
}

Status Project::save() const {
    if (m_file.empty()) return makeError("project has no file path");
    return json::saveFile(m_file, json::toPlain(settings));
}

Status Project::saveAs(const std::filesystem::path& file) {
    m_file = file;
    return save();
}

} // namespace ox
