#pragma once

#include <oxwald/core/result.hpp>
#include <oxwald/runtime/input.hpp>

#include <glm/vec3.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Game projects: a directory with a "<Name>.oxproj" JSON file (plain JSON of ProjectSettings) next to the asset
// directories. The engine mounts the directory as project://.
namespace ox {

struct ProjectPhysicsSettings {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    f32 fixedRate = 60.0f; // Hz of FixedUpdate (physics + gameplay logic)
    u32 maxSubsteps = 8;   // fixed steps per frame before time is dropped
    u32 maxBodies = 65536;
};

struct ProjectAudioSettings {
    u32 sampleRate = 48000;
    u32 maxVoices = 64;
    std::map<std::string, f32> busVolumes; // default bus volumes ("Music": 0.8)
};

struct ProjectRenderingSettings {
    std::map<std::string, std::string> cvars; // project defaults applied before user settings ("r.Bloom": "1")
    bool rayTracingIfSupported = false;
    std::string upscaler = "Off";
};

struct ProjectPackagingSettings {
    std::vector<std::string> alwaysIncludeAssets; // URIs or globs cooked even when unreferenced
    std::vector<std::string> targetPlatforms{"macos", "windows", "linux"};
    std::string outputDir = "build/package";
    bool compress = true;
};

struct ProjectSettings {
    std::string name = "Untitled";
    std::string version = "0.1.0"; // game version (stored in save headers)
    std::string company;
    std::string startupScene;                    // e.g. "project://levels/main.oxscene"
    std::vector<std::string> assetDirs{"assets"}; // relative to the project root
    std::map<std::string, bool> modules;          // module toggles ("physics": true); missing = enabled
    u32 saveVersion = 1;                          // current save-game data version (migrations upgrade to it)
    InputMappingConfig input;                     // default input mappings
    ProjectPhysicsSettings physics;
    ProjectAudioSettings audio;
    ProjectRenderingSettings rendering;
    std::string defaultQuality = "High";          // scalability default (Low/Medium/High/Ultra)
    std::map<std::string, std::string> scalability; // per-group defaults ("Shadows": "Medium")
    ProjectPackagingSettings packaging;

    [[nodiscard]] bool moduleEnabled(std::string_view module) const;
};

class Project {
public:
    static constexpr std::string_view kExtension = ".oxproj";

    Project() = default;
    // A .oxproj file or a directory containing exactly one (the first in name order is used otherwise).
    [[nodiscard]] static Result<Project> load(const std::filesystem::path& fileOrDirectory);
    // New project file "<dir>/<name>.oxproj" (not written until save()).
    [[nodiscard]] static Project create(const std::filesystem::path& directory, std::string name);

    Status save() const;
    Status saveAs(const std::filesystem::path& file);

    [[nodiscard]] const std::filesystem::path& file() const { return m_file; }
    [[nodiscard]] std::filesystem::path root() const { return m_file.parent_path(); }

    ProjectSettings settings;

private:
    std::filesystem::path m_file;
};

// Reflection for project/settings types (idempotent; also registers input types).
void registerProjectTypes();

} // namespace ox
