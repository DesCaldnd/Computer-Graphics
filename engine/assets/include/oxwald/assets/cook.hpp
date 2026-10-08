#pragma once

// Game packaging: cooks the assets reachable from the startup scenes (+ always-include list) into a .oxpak.
// Used by tools/oxpack and the runtime's packaging step.

#include <oxwald/assets/asset_registry.hpp>
#include <oxwald/assets/pak.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace ox::assets {

struct CookOptions {
    std::vector<std::string> startupScenes; // asset paths relative to Assets/ (or UUID strings)
    std::vector<std::string> alwaysInclude; // asset paths, UUIDs or directory prefixes ("UI/")
    bool includeAll = false;                // everything in the project (default when both lists are empty)
    bool compress = true;                   // zstd per entry (textures stay uncompressed for range reads)
    i32 compressionLevel = 6;
    u32 alignment = 16;
};

struct CookReport {
    struct Item {
        Uuid uuid;
        AssetType type = AssetType::Unknown;
        std::string path;
        u64 size = 0;
        u64 storedSize = 0;
    };
    std::vector<Item> items;
    std::vector<std::string> errors;
    u64 totalSize = 0;
    u64 totalStored = 0;
    u64 pakSize = 0;
};

// Reads <project>/pack.json when present: {"startupScenes": [...], "alwaysInclude": [...]} (merged with options).
[[nodiscard]] Result<CookReport> cookProject(AssetRegistry& registry, const std::filesystem::path& outputPak,
                                             CookOptions options = {});

} // namespace ox::assets
