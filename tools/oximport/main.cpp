// oximport — imports one source file with the engine importers and prints what the asset pipeline produces.
//
//   oximport <file> [--settings '<json>'] [--out <dir>] [--json]
//
// --settings  importer settings (merged over the defaults, same keys as in .meta files)
// --out       writes every artifact as <dir>/<name or "main"><ext>
// --json      machine-readable summary
#include <oxwald/assets/assets.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/time.hpp>

#include <cstdio>
#include <filesystem>
#include <iostream>

using namespace ox;
using namespace ox::assets;

namespace {

void usage() {
    std::fprintf(stderr, "usage: oximport <file> [--settings '<json>'] [--out <dir>] [--json]\n");
}

nlohmann::ordered_json describe(const ImportedArtifact& a) {
    nlohmann::ordered_json j;
    j["name"] = a.name.empty() ? "main" : a.name;
    j["uuid"] = a.uuid.toString();
    j["type"] = std::string(assetTypeName(a.type));
    j["bytes"] = a.data.size();
    if (a.type == AssetType::Mesh) {
        if (auto m = deserializeMesh(a.data)) {
            j["vertices"] = m->vertexCount();
            auto& lods = j["triangles"] = nlohmann::ordered_json::array();
            for (u32 l = 0; l < m->lodCount(); ++l) lods.push_back(m->triangleCount(l));
            j["submeshes"] = m->submeshes.size();
            j["meshlets"] = m->meshlets.size();
            j["skinned"] = m->skinned();
            j["bounds"] = {{m->bounds.min.x, m->bounds.min.y, m->bounds.min.z}, {m->bounds.max.x, m->bounds.max.y, m->bounds.max.z}};
            j["hullVertices"] = m->collision.hullVertices.size();
        }
    } else if (a.type == AssetType::Texture) {
        if (auto t = readTextureInfo(a.data)) {
            j["format"] = std::string(textureFormatName(t->desc.format));
            j["size"] = {t->desc.width, t->desc.height};
            j["layers"] = t->desc.layers;
            j["mips"] = t->desc.mipCount;
        }
    } else if (a.type == AssetType::Material) {
        if (auto m = loadMaterial(a.data)) {
            j["textures"] = m->textureDependencies().size();
        }
    }
    if (!a.dependencies.empty()) j["dependencies"] = a.dependencies.size();
    if (!a.info.empty()) j["info"] = a.info;
    return j;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path input, outDir;
    nlohmann::ordered_json settings = nlohmann::ordered_json::object();
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--settings" && i + 1 < argc) {
            settings = nlohmann::ordered_json::parse(argv[++i], nullptr, false);
            if (settings.is_discarded() || !settings.is_object()) {
                std::fprintf(stderr, "oximport: --settings is not a JSON object\n");
                return 2;
            }
        } else if (arg == "--out" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (input.empty()) {
            input = arg;
        } else {
            usage();
            return 2;
        }
    }
    if (input.empty()) {
        usage();
        return 2;
    }
    registerAssetTypes();
    ImporterRegistry importers;
    importers.addBuiltins();
    IAssetImporter* importer = importers.findForPath(input);
    if (!importer) {
        std::fprintf(stderr, "oximport: no importer for %s\n", input.string().c_str());
        return 1;
    }
    Stopwatch sw(true);
    auto artifacts = importStandalone(importers, input, settings);
    const f64 ms = sw.elapsedMs();
    if (!artifacts) {
        std::fprintf(stderr, "oximport: %s\n", artifacts.error().message.c_str());
        return 1;
    }
    nlohmann::ordered_json summary;
    summary["source"] = input.generic_string();
    summary["importer"] = std::string(importer->name());
    summary["timeMs"] = ms;
    auto& arr = summary["artifacts"] = nlohmann::ordered_json::array();
    for (const auto& a : *artifacts) arr.push_back(describe(a));
    if (!outDir.empty()) {
        std::filesystem::create_directories(outDir);
        for (const auto& a : *artifacts) {
            std::string name = a.name.empty() ? "main" : a.name;
            std::replace(name.begin(), name.end(), '/', '_');
            const auto path = outDir / (name + std::string(artifactExtension(a.type)));
            std::FILE* f = std::fopen(path.string().c_str(), "wb");
            if (!f) {
                std::fprintf(stderr, "oximport: cannot write %s\n", path.string().c_str());
                return 1;
            }
            std::fwrite(a.data.data(), 1, a.data.size(), f);
            std::fclose(f);
        }
    }
    if (json) {
        std::cout << summary.dump(2) << "\n";
        return 0;
    }
    std::printf("%s  (importer: %s, %.1f ms)\n", input.string().c_str(), importer->name().data(), ms);
    for (const auto& j : arr) {
        std::printf("  %-10s %-24s %10llu bytes", j["type"].get<std::string>().c_str(), j["name"].get<std::string>().c_str(),
                    static_cast<unsigned long long>(j["bytes"].get<u64>()));
        if (j.contains("vertices")) {
            std::printf("  %u vertices, triangles/LOD %s, %zu meshlets", j["vertices"].get<u32>(), j["triangles"].dump().c_str(),
                        j["meshlets"].get<usize>());
        }
        if (j.contains("format")) {
            std::printf("  %s %ux%u x%u, %u mips", j["format"].get<std::string>().c_str(), j["size"][0].get<u32>(),
                        j["size"][1].get<u32>(), j["layers"].get<u32>(), j["mips"].get<u32>());
        }
        if (j.contains("dependencies")) std::printf("  deps: %zu", j["dependencies"].get<usize>());
        std::printf("\n");
    }
    return 0;
}
