// oxpack — cooks a project's assets into a .oxpak archive for the game runtime.
//
//   oxpack <project dir> -o Game.oxpak [--scene <asset path>]... [--include <path|dir/|uuid>]... [--all]
//          [--no-compress] [--level <zstd level>] [--align <bytes>] [--verify] [--quiet]
//
// Without --scene/--include (and without <project>/pack.json) every asset is packed. Assets reachable from
// the startup scenes (dependencies: prefabs, meshes, materials, textures, ...) are always included.
#include <oxwald/assets/assets.hpp>
#include <oxwald/core/log.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>

using namespace ox;
using namespace ox::assets;

namespace {
void usage() {
    std::fprintf(stderr,
                 "usage: oxpack <project dir> -o <out.oxpak> [--scene <path>]... [--include <path>]... [--all]\n"
                 "              [--no-compress] [--level <n>] [--align <bytes>] [--verify] [--quiet]\n");
}

std::string human(u64 bytes) {
    char buf[32];
    if (bytes >= (1u << 20)) std::snprintf(buf, sizeof(buf), "%.2f MiB", f64(bytes) / f64(1u << 20));
    else if (bytes >= 1024) std::snprintf(buf, sizeof(buf), "%.1f KiB", f64(bytes) / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}
} // namespace

int main(int argc, char** argv) {
    std::filesystem::path project, output;
    CookOptions options;
    bool verify = false, quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) output = next();
        else if (arg == "--scene" && i + 1 < argc) options.startupScenes.push_back(next());
        else if (arg == "--include" && i + 1 < argc) options.alwaysInclude.push_back(next());
        else if (arg == "--all") options.includeAll = true;
        else if (arg == "--no-compress") options.compress = false;
        else if (arg == "--level" && i + 1 < argc) options.compressionLevel = std::atoi(next());
        else if (arg == "--align" && i + 1 < argc) options.alignment = u32(std::max(1, std::atoi(next())));
        else if (arg == "--verify") verify = true;
        else if (arg == "--quiet") quiet = true;
        else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (project.empty() && !arg.starts_with("-")) project = arg;
        else {
            usage();
            return 2;
        }
    }
    if (project.empty() || output.empty()) {
        usage();
        return 2;
    }
    if (!std::filesystem::is_directory(project / "Assets")) {
        std::fprintf(stderr, "oxpack: %s has no Assets/ directory\n", project.string().c_str());
        return 1;
    }
    if (quiet) log::setMinLevel(log::Level::Warn);
    AssetRegistry registry(project);
    auto report = cookProject(registry, output, options);
    if (!report) {
        std::fprintf(stderr, "oxpack: %s\n", report.error().message.c_str());
        return 1;
    }
    auto items = report->items;
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.size > b.size; });
    if (!quiet) {
        std::printf("%-14s %12s %12s  %s\n", "type", "size", "stored", "asset");
        for (const auto& it : items) {
            std::printf("%-14s %12s %12s  %s\n", std::string(assetTypeName(it.type)).c_str(), human(it.size).c_str(),
                        human(it.storedSize).c_str(), it.path.c_str());
        }
    }
    std::printf("%zu assets, %s -> %s stored, pak %s: %s\n", items.size(), human(report->totalSize).c_str(),
                human(report->totalStored).c_str(), human(report->pakSize).c_str(), output.string().c_str());
    int rc = 0;
    for (const auto& e : report->errors) {
        std::fprintf(stderr, "oxpack: error: %s\n", e.c_str());
        rc = 1;
    }
    if (verify) {
        auto reader = PakReader::open(output);
        if (!reader) {
            std::fprintf(stderr, "oxpack: verify: %s\n", reader.error().message.c_str());
            return 1;
        }
        auto bad = (*reader)->verify();
        PakAssetSource source;
        auto st = source.addPak(*reader);
        if (!bad.empty() || !st) {
            for (const auto& b : bad) std::fprintf(stderr, "oxpack: verify: corrupt entry %s\n", b.c_str());
            if (!st) std::fprintf(stderr, "oxpack: verify: %s\n", st.error().message.c_str());
            return 1;
        }
        std::printf("verify: ok (%zu entries, %zu assets)\n", (*reader)->entries().size(), source.allAssets().size());
    }
    return rc;
}
