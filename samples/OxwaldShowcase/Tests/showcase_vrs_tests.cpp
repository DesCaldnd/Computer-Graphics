// Variable rate shading measurements on the showcase stations (docs/dev/perf.md, "Variable rate shading").
// Opt-in, needs a GPU with VK_KHR_fragment_shading_rate:
//   OX_VRS_PERF=1 OX_VULKAN_VALIDATION=0 ox_showcase_tests --gtest_filter=ShowcaseVrs.*
//   OX_VRS_PERF=01_Lighting,06_World   only these stations;  OX_VRS_RES=1920x1080 only this size
//   OX_VRS_OUT=E:/somewhere            output directory (CSV + PNGs)
//   OX_VRS_CONFIGS=Adaptive_t002_max2,PanMotion   only these configurations (plus the Off references)
// For every station, size and configuration a fresh headless engine runs the station in photo mode with a fixed
// 1/60 s step (identical animation state across runs), averages GPU timings over frames 121–180 and grabs frame 181.
// Quality = PSNR / SSIM / FLIP of that frame against the VRS-off frame; "Off (repeat)" measures the run-to-run noise.
#include "showcase.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/render/features/vrs/vrs.hpp>
#include <oxwald/render/renderer.hpp>
#include <oxwald/render/runtime_renderer.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/runtime/runtime.hpp>
#include <oxwald/ui/ui_module.hpp>

#include "vrs_image_metrics.hpp"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ox;

namespace {

const fs::path kProject = OX_SHOWCASE_DIR;

struct Config {
    std::string name;
    std::vector<std::string> cvars;
    bool pan = false; // camera yaw 0.6°/frame (≈ 21 px/frame at 1080p with a 60° FOV)
};

struct RunResult {
    double frameMs = 0, affectedMs = 0, vrsMs = 0;
    std::map<std::string, double> passMs;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    double coarse = 0; // fraction of tiles coarser than 1x1 (last frame)
    bool ok = false;
};

bool affected(const std::string& pass) {
    const auto& list = render::vrs::defaultPasses();
    return std::find(list.begin(), list.end(), pass) != list.end();
}

RunResult runStation(const std::string& station, uint32_t w, uint32_t h, const Config& cfg, const fs::path& outDir) {
    RunResult r;
    Engine engine;
    engine.addModule(std::make_unique<ui::UiModule>());
    render::RuntimeRendererOptions ro;
    ro.headlessWidth = w;
    ro.headlessHeight = h;
    std::unique_ptr<IRenderer> inner = render::createRenderer(ro);
    IRenderer* innerRenderer = inner.get(); // withUi() wraps it: rendererOf() needs the runtime renderer itself
    engine.setRenderer(ui::withUi(std::move(inner)));
    EngineConfig ec;
    ec.appName = "ShowcaseVrs";
    ec.projectPath = kProject;
    ec.headless = true;
    ec.startupScene = "project://Assets/Scenes/Stations/" + station + ".oxscene";
    ec.userDir = outDir / "user";
    ec.loadUserSettings = false;
    ec.saveUserSettingsOnShutdown = false;
    ec.threadedRendering = false;
    ec.quality = QualityLevel::High;
    ec.cvars = cfg.cvars;
    ec.gameArgs = {"--photo"};
    if (Status s = engine.init(ec); !s) {
        ADD_FAILURE() << station << ": " << s.error().message;
        return r;
    }
    render::Renderer* renderer = render::rendererOf(*innerRenderer);
    if (!renderer) {
        ADD_FAILURE() << "no render::Renderer";
        return r;
    }
    auto panCamera = [&](int frame) {
        if (!cfg.pan || frame < 60) return;
        World& world = engine.world();
        for (auto [e, cam] : world.view<CameraComponent>().each()) {
            if (!cam.primary) continue; // photo mode makes TourCam.A primary
            Entity ent = world.wrap(e);
            ent.setRotation(glm::angleAxis(glm::radians(0.6f), glm::vec3(0, 1, 0)) * ent.localTransform().rotation);
            break;
        }
    };
    constexpr int kWarmup = 120, kMeasure = 60;
    int frame = 0;
    for (; frame < kWarmup; ++frame) {
        panCamera(frame);
        engine.tick(1.0 / 60.0);
    }
    // Medians over the measured frames (other GPU work on the machine adds outliers).
    std::vector<double> frameV, affectedV, vrsV;
    std::map<std::string, std::vector<double>> passV;
    for (; frame < kWarmup + kMeasure; ++frame) {
        panCamera(frame);
        engine.tick(1.0 / 60.0);
        const render::RenderStats& st = renderer->stats();
        if (st.gpuFrameMs <= 0) continue;
        double aff = 0, vrs = 0;
        std::map<std::string, double> perPass;
        for (const render::PassTiming& p : st.passes) {
            const std::string name = p.name.substr(p.name.find('/') + 1);
            perPass[name] += p.gpuMs;
            if (affected(name)) aff += p.gpuMs;
            if (name.starts_with("VRS.")) vrs += p.gpuMs;
        }
        frameV.push_back(st.gpuFrameMs);
        affectedV.push_back(aff);
        vrsV.push_back(vrs);
        for (auto& [k, v] : perPass) passV[k].push_back(v);
    }
    auto median = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    r.frameMs = median(frameV);
    r.affectedMs = median(affectedV);
    r.vrsMs = median(vrsV);
    for (auto& [k, v] : passV) r.passMs[k] = median(v);
    // Coarse fraction of the rate image the main view used.
    render::RenderView* v = nullptr;
    for (render::ViewId id = 0; id < 8 && !v; ++id) v = renderer->view(id);
    if (v) {
        rhi::RenderGraph& g = v->graph();
        for (u32 i = 0; i < g.passCount(); ++i) {
            if (g.passName(i) != "ForwardOpaque" || !g.passShadingRate(i).valid()) continue;
            renderer->device().waitIdle();
            const std::vector<u8> rate = renderer->device().readTexture(g.physicalTexture(g.passShadingRate(i)));
            r.coarse = rate.empty() ? 0.0 : double(std::count_if(rate.begin(), rate.end(), [](u8 x) { return x != 0; })) / double(rate.size());
        }
    }
    const fs::path png = outDir / (station + "_" + std::to_string(w) + "_" + cfg.name + ".png");
    bool written = false;
    panCamera(frame);
    if (!engine.renderer().requestScreenshot(png, [&](bool ok) { written = ok; })) {
        ADD_FAILURE() << "screenshot not supported";
        return r;
    }
    engine.tick(1.0 / 60.0);
    engine.tick(1.0 / 60.0);
    engine.shutdown();
    if (!written) {
        ADD_FAILURE() << "screenshot failed " << png;
        return r;
    }
    int iw = 0, ih = 0, comp = 0;
    stbi_uc* data = stbi_load(png.string().c_str(), &iw, &ih, &comp, 4);
    if (!data) {
        ADD_FAILURE() << "cannot read " << png;
        return r;
    }
    r.w = uint32_t(iw);
    r.h = uint32_t(ih);
    r.rgba.assign(data, data + size_t(iw) * ih * 4);
    stbi_image_free(data);
    r.ok = true;
    return r;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

} // namespace

TEST(ShowcaseVrs, StationsQualityAndPerformance) {
    const char* env = std::getenv("OX_VRS_PERF");
    if (!env || !*env || std::string(env) == "0") GTEST_SKIP() << "set OX_VRS_PERF=1 (and OX_VULKAN_VALIDATION=0)";
    std::vector<std::string> stations = {"01_Lighting", "02_Materials", "04_Volumetrics", "05_Water", "06_World"};
    if (std::string(env) != "1") stations = split(env);
    std::vector<std::pair<uint32_t, uint32_t>> sizes = {{1920, 1080}, {3840, 2160}};
    if (const char* res = std::getenv("OX_VRS_RES")) {
        uint32_t w = 0, h = 0;
        if (std::sscanf(res, "%ux%u", &w, &h) == 2) sizes = {{w, h}};
    }
    const std::vector<Config> configs = {
        {"Off", {"r.VRS=0"}},
        {"OffRepeat", {"r.VRS=0"}},
        {"Adaptive_t002_max2", {"r.VRS=1", "r.VRS.Mode=Adaptive", "r.VRS.Threshold=0.02", "r.VRS.MaxRate=2"}},
        {"Adaptive_t003_max2", {"r.VRS=1", "r.VRS.Mode=Adaptive", "r.VRS.Threshold=0.03", "r.VRS.MaxRate=2"}},
        {"Adaptive_t005_max2", {"r.VRS=1", "r.VRS.Mode=Adaptive", "r.VRS.Threshold=0.05", "r.VRS.MaxRate=2"}},
        {"Adaptive_t005_max4", {"r.VRS=1", "r.VRS.Mode=Adaptive", "r.VRS.Threshold=0.05", "r.VRS.MaxRate=4"}},
        {"Adaptive_t008_max2", {"r.VRS=1", "r.VRS.Mode=Adaptive", "r.VRS.Threshold=0.08", "r.VRS.MaxRate=2"}},
        {"Motion", {"r.VRS=1", "r.VRS.Mode=Motion", "r.VRS.MaxRate=4"}},
        {"Foveated", {"r.VRS=1", "r.VRS.Mode=Foveated", "r.VRS.MaxRate=4"}},
    };
    const std::vector<Config> panConfigs = {
        {"PanOff", {"r.VRS=0"}, true},
        {"PanAdaptive_t003_max2", {"r.VRS=1", "r.VRS.Mode=Adaptive", "r.VRS.Threshold=0.03", "r.VRS.MaxRate=2"}, true},
        {"PanMotion", {"r.VRS=1", "r.VRS.Mode=Motion", "r.VRS.MaxRate=4"}, true},
    };
    // OX_VRS_OUT: output directory (PNGs of every run, ~1 GB with 4K; default <temp>/oxwald_vrs_perf).
    const char* outEnv = std::getenv("OX_VRS_OUT");
    std::vector<std::string> onlyConfigs;
    if (const char* cfgs = std::getenv("OX_VRS_CONFIGS")) onlyConfigs = split(cfgs);
    const fs::path outDir = outEnv && *outEnv ? fs::path(outEnv) : fs::temp_directory_path() / "oxwald_vrs_perf";
    fs::create_directories(outDir);
    std::ofstream csv(outDir / "vrs_results.csv");
    csv << "station,width,height,config,gpu_frame_ms,affected_ms,vrs_passes_ms,coarse_tiles,psnr_db,ssim,flip,"
           "forward_ms,translucency_ms,vrs_generate_ms,vrs_analyze_ms,vrs_analyze_opaque_ms\n";
    for (const std::string& station : stations) {
        for (auto [w, h] : sizes) {
            for (const std::vector<Config>* set : {&configs, &panConfigs}) {
                RunResult ref;
                for (const Config& c : *set) {
                    // OX_VRS_CONFIGS: only these configurations (the reference, first of each set, always runs).
                    if (!onlyConfigs.empty() && &c != &set->front() &&
                        std::find(onlyConfigs.begin(), onlyConfigs.end(), c.name) == onlyConfigs.end()) {
                        continue;
                    }
                    RunResult r = runStation(station, w, h, c, outDir);
                    if (!r.ok) continue;
                    if (&c == &set->front()) ref = r;
                    double p = 99, s = 1, f = 0;
                    if (ref.ok && &c != &set->front() && ref.w == r.w && ref.h == r.h) {
                        // The HUD's FPS / GPU ms counter (bottom right) differs from run to run: not part of the
                        // comparison.
                        for (uint32_t y = r.h - r.h / 16; y < r.h; ++y) {
                            for (uint32_t x = r.w - r.w / 4; x < r.w; ++x) {
                                const size_t i = (size_t(y) * r.w + x) * 4;
                                std::copy_n(ref.rgba.begin() + i, 4, r.rgba.begin() + i);
                            }
                        }
                        const vrs_metrics::ImageView a{ref.w, ref.h, ref.rgba.data()}, b{r.w, r.h, r.rgba.data()};
                        p = vrs_metrics::psnr(a, b);
                        s = vrs_metrics::ssim(a, b);
                        f = vrs_metrics::flip(a, b);
                    }
                    const double fwd = r.passMs["ForwardOpaque"] + r.passMs["World.Forward"];
                    const double tr = r.passMs["Translucency.Refractive"] + r.passMs["Translucency.OIT"] +
                                      r.passMs["Translucency.Sorted"] + r.passMs["Water.Surface"] + r.passMs["Particles.Render"];
                    std::printf("[VRS] %s %ux%u %-24s frame %6.2f ms  affected %6.2f ms  vrs %5.3f ms  coarse %5.1f %%  "
                                "PSNR %6.2f  SSIM %.4f  FLIP %.4f\n",
                                station.c_str(), w, h, c.name.c_str(), r.frameMs, r.affectedMs, r.vrsMs, r.coarse * 100.0, p,
                                s, f);
                    std::fflush(stdout);
                    csv << station << ',' << w << ',' << h << ',' << c.name << ',' << r.frameMs << ',' << r.affectedMs << ','
                        << r.vrsMs << ',' << r.coarse << ',' << p << ',' << s << ',' << f << ',' << fwd << ',' << tr << ','
                        << r.passMs["VRS.Generate"] << ',' << r.passMs["VRS.Analyze"] << ','
                        << r.passMs["VRS.AnalyzeOpaque"] << '\n';
                    csv.flush();
                }
            }
        }
    }
}
