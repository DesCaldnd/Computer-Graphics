// oxshowcase_generate [--project <dir>] [--only <scene id>] — regenerates the OxwaldShowcase content.
#include "gen.hpp"

#include <oxwald/assets/asset_registry.hpp>
#include <oxwald/assets/asset_types.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/render/register_types.hpp>
#include <oxwald/runtime/project.hpp>
#include <oxwald/scene/scene.hpp>

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

using namespace ox;
using namespace ox::showcase::gen;

namespace {

int run(int argc, char** argv) {
    fs::path project = OX_SHOWCASE_DIR;
    std::string only;
    bool check = false;
    bool skipImports = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--project" && i + 1 < argc) project = argv[++i];
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--check") check = true;
        else if (a == "--help") {
            std::printf("usage: oxshowcase_generate [--project DIR] [--only menu|hub|<station id>] [--check]\n"
                        "  --check  write nothing; exit 1 when the committed content differs from what would be generated\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    (void)skipImports;
    registerSceneTypes();
    registerGameplayTypes();
    render::registerRenderTypes();
    assets::registerAssetTypes();
    registerProjectTypes();

    Gen gen(project);
    gen.checkOnly = check;
    // Generated sources first (their metas get deterministic ids), then metas for everything else, then content
    // that references imported sub-assets.
    generateTextures(gen);
    generateAudio(gen);
    generateMannequin(gen);
    gen.scanAndMetaEverything();
    generateMaterials(gen);
    generateProjectFile(gen);
    generatePrefabs(gen);
    generateData(gen);
    gen.registry->scan();

    struct Build {
        const char* id;
        void (*fn)(Gen&);
    };
    const Build builds[] = {{"menu", buildMainMenu},       {"hub", buildHub},
                            {"lighting", buildLighting},   {"materials", buildMaterials},
                            {"reflections", buildReflections}, {"volumetrics", buildVolumetrics},
                            {"water", buildWater},         {"world", buildWorld},
                            {"physics", buildPhysics},     {"animation", buildAnimation},
                            {"ai", buildAI},               {"splines", buildSplines},
                            {"audio", buildAudio},         {"network", buildNetwork},
                            {"rtx", buildRtx},             {"saves", buildSaves}};
    for (const Build& b : builds) {
        if (!only.empty() && only != b.id) continue;
        b.fn(gen);
    }
    gen.registry->scan();
    if (check) {
        for (const std::string& d : gen.differences) std::printf("differs: %s\n", d.c_str());
        std::printf("oxshowcase_generate --check: %zu differences, %u files up to date (%u of them within the numeric tolerance)\n",
                    gen.differences.size(), gen.filesUnchanged, gen.filesEquivalent);
        return gen.differences.empty() ? 0 : 1;
    }
    std::printf("oxshowcase_generate: %u files written, %u unchanged\n", gen.filesWritten, gen.filesUnchanged);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // An escaping exception (std::filesystem mostly) would end in a silent fast-fail on Windows: name it instead.
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "oxshowcase_generate: %s\n", e.what());
        return 3;
    }
}
