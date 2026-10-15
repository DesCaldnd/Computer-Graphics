// oximport — imports one source file with the engine importers and prints what the asset pipeline produces.
//
//   oximport <file> [--settings '<json>'] [--out <dir>] [--json] [--project <dir>]
//   oximport --ntc --albedo a.png [--normal n.png] [--orm o.png] [--emissive e.png] [--settings '<json>'] [--out <dir>]
//
// --settings  importer settings (merged over the defaults, same keys as in .meta files)
// --out       writes every artifact as <dir>/<name or "main"><ext>
// --json      machine-readable summary
// --project   imports <file> inside that project (other assets resolve, e.g. the textures of a material for
//             neural compression: --settings '{"neuralCompression": "On", "neuralPreset": "Medium"}'). The metas
//             are not modified; dependencies are imported into the project's cache as usual.
// --ntc       neural texture compression of loose images (assets/neural/ntc.hpp) with the material importer's
//             neural* settings keys; prints size and PSNR/SSIM per channel against BC7/BC5, writes main.oxntc.
#include <oxwald/assets/assets.hpp>
#include <oxwald/assets/neural/ntc.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/time.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>

using namespace ox;
using namespace ox::assets;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: oximport <file> [--settings '<json>'] [--out <dir>] [--json] [--project <dir>]\n"
                 "       oximport --ntc --albedo <png> [--normal <png>] [--orm <png>] [--emissive <png>] "
                 "[--settings '<json>'] [--out <dir>] [--json]\n");
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

void printNtc(const NeuralTextureSet& set, const NtcStats& st, f64 ms) {
    std::printf("NeuralTextureSet %ux%u, preset %s, %u neural mips + %u BC mips, hidden %u\n", set.width, set.height,
                ntcPresetName(set.preset).data(), set.neuralMips, set.mipCount - set.neuralMips, set.hidden());
    std::printf("  payload %llu B (latents %llu, network %llu, fallback %llu) = %.2f bits/texel; BC7/BC5 baseline %llu B "
                "(%.1fx); GPU memory with inference %llu B\n",
                (unsigned long long)set.payloadBytes(), (unsigned long long)set.latentBytes(),
                (unsigned long long)set.networkBytes(), (unsigned long long)set.fallbackBytes(), set.bitsPerTexel(),
                (unsigned long long)st.sourceBcBytes, st.sourceBcBytes ? f64(st.sourceBcBytes) / f64(set.payloadBytes()) : 0.0,
                (unsigned long long)set.inferenceVramBytes());
    for (const NtcChannelStats& c : st.channels)
        std::printf("  %-10s NTC %6.2f dB  SSIM %.4f   |  BC %6.2f dB  SSIM %.4f\n", c.name.c_str(), c.psnr, c.ssim, c.bcPsnr,
                    c.bcSsim);
    std::printf("  training: %u steps, %.0f ms (total %.0f ms)\n", set.stats.steps, set.stats.trainMs, ms);
}

nlohmann::ordered_json ntcJson(const NeuralTextureSet& set, const NtcStats& st) {
    nlohmann::ordered_json j;
    j["size"] = {set.width, set.height};
    j["preset"] = std::string(ntcPresetName(set.preset));
    j["neuralMips"] = set.neuralMips;
    j["payloadBytes"] = set.payloadBytes();
    j["latentBytes"] = set.latentBytes();
    j["networkBytes"] = set.networkBytes();
    j["fallbackBytes"] = set.fallbackBytes();
    j["inferenceVramBytes"] = set.inferenceVramBytes();
    j["bcBytes"] = st.sourceBcBytes;
    j["bitsPerTexel"] = set.bitsPerTexel();
    j["trainMs"] = set.stats.trainMs;
    for (const NtcChannelStats& c : st.channels)
        j["channels"][c.name] = {{"psnr", c.psnr}, {"ssim", c.ssim}, {"bcPsnr", c.bcPsnr}, {"bcSsim", c.bcSsim}};
    return j;
}

bool writeFile(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return false;
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path input, outDir, project;
    std::filesystem::path ntcImages[4];
    nlohmann::ordered_json settings = nlohmann::ordered_json::object();
    bool json = false, ntc = false;
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
        } else if (arg == "--project" && i + 1 < argc) {
            project = argv[++i];
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--ntc") {
            ntc = true;
        } else if ((arg == "--albedo" || arg == "--normal" || arg == "--orm" || arg == "--emissive") && i + 1 < argc) {
            const int slot = arg == "--albedo" ? 0 : arg == "--normal" ? 1 : arg == "--orm" ? 2 : 3;
            ntcImages[slot] = argv[++i];
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
    registerAssetTypes();

    if (ntc) {
        NtcSourceImages src;
        Image* imgs[4] = {&src.albedo, &src.normal, &src.orm, &src.emissive};
        for (int s = 0; s < 4; ++s) {
            if (ntcImages[s].empty()) continue;
            auto img = loadImage(ntcImages[s]);
            if (!img) {
                std::fprintf(stderr, "oximport: %s\n", img.error().message.c_str());
                return 1;
            }
            *imgs[s] = std::move(*img);
        }
        MaterialImportSettings ms;
        fromSettingsJson(settings, ms);
        Stopwatch sw(true);
        int lastPct = -1;
        auto set = encodeNeuralTextures(src, ms.toNeuralSettings(), [&](f32 f, f32 loss) {
            const int pct = int(f * 10.0f) * 10;
            if (!json && pct != lastPct) {
                lastPct = pct;
                std::fprintf(stderr, "  training %3d%%  loss %.6f\n", pct, loss);
            }
        });
        if (!set) {
            std::fprintf(stderr, "oximport: %s\n", set.error().message.c_str());
            return 1;
        }
        const NtcStats st = evaluateNeuralTextures(*set, src, true);
        set->stats.channels = st.channels;
        set->stats.sourceBcBytes = st.sourceBcBytes;
        const f64 ms2 = sw.elapsedMs();
        if (!outDir.empty()) {
            std::filesystem::create_directories(outDir);
            if (!writeFile(outDir / "main.oxntc", serializeNeuralTextureSet(*set))) {
                std::fprintf(stderr, "oximport: cannot write %s\n", (outDir / "main.oxntc").string().c_str());
                return 1;
            }
            const auto decoded = decodeNeuralMip(*set, 0);
            const char* names[4] = {"albedo", "normal", "orm", "emissive"};
            for (int s = 0; s < 4; ++s)
                if (!decoded[s].empty()) (void)saveImagePng(outDir / (std::string("decoded_") + names[s] + ".png"), decoded[s]);
        }
        if (json) std::cout << ntcJson(*set, st).dump(2) << "\n";
        else printNtc(*set, st, ms2);
        return 0;
    }

    if (input.empty()) {
        usage();
        return 2;
    }
    std::unique_ptr<AssetRegistry> registry;
    ImporterRegistry standaloneImporters;
    standaloneImporters.addBuiltins();
    if (!project.empty()) {
        registry = std::make_unique<AssetRegistry>(project);
        registry->scan();
    }
    const ImporterRegistry& importers = registry ? registry->importers() : standaloneImporters;
    IAssetImporter* importer = importers.findForPath(input);
    if (!importer) {
        std::fprintf(stderr, "oximport: no importer for %s\n", input.string().c_str());
        return 1;
    }
    Stopwatch sw(true);
    Result<std::vector<ImportedArtifact>> artifacts = makeError("not imported");
    if (registry) {
        // Inside the project: the source keeps its UUID, other assets resolve through the registry; the meta file
        // is left untouched (settings from the command line are merged over it).
        const std::filesystem::path abs = std::filesystem::absolute(input);
        const std::string rel = std::filesystem::relative(abs, registry->assetsDir()).generic_string();
        const auto uuid = registry->uuidForPath(rel);
        auto meta = uuid ? registry->meta(*uuid) : std::nullopt;
        if (!meta) {
            std::fprintf(stderr, "oximport: %s is not an asset of %s\n", input.string().c_str(), project.string().c_str());
            return 1;
        }
        meta->settings.merge_patch(settings);
        ImportContext::Callbacks cb;
        cb.resolve = [&](const std::filesystem::path& p, const nlohmann::ordered_json&) {
            return registry->uuidForPath(std::filesystem::relative(p, registry->assetsDir()).generic_string());
        };
        cb.readArtifact = [&](const Uuid& id) { return registry->readArtifact(id); };
        cb.sourceOf = [&](const Uuid& id) -> std::optional<std::pair<std::filesystem::path, nlohmann::ordered_json>> {
            auto m = registry->meta(id);
            if (!m) return std::nullopt;
            return std::make_pair(registry->absolutePath(id), m->settings);
        };
        ImportContext ctx(abs, rel, *meta, cb);
        if (auto st = importer->import(ctx); !st) artifacts = st.error();
        else artifacts = std::move(ctx.artifacts());
    } else {
        artifacts = importStandalone(importers, input, settings);
    }
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
            if (!writeFile(path, a.data)) {
                std::fprintf(stderr, "oximport: cannot write %s\n", path.string().c_str());
                return 1;
            }
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
        if (j["type"] == "NeuralTextureSet" && j.contains("info")) {
            const auto& info = j["info"];
            std::printf("  %.2f bits/texel, %llu B vs BC %llu B", info.value("bitsPerTexel", 0.0),
                        static_cast<unsigned long long>(info.value("bytes", u64(0))),
                        static_cast<unsigned long long>(info.value("bcBytes", u64(0))));
        }
        std::printf("\n");
    }
    return 0;
}
