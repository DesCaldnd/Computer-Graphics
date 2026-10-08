// GPU tests of the UI overlay feature (labels ui;gpu): ImGui and RmlUi rendered offscreen through the real renderer
// (Overlay injection point, output resolution) and compared with goldens in engine/ui/tests/data/golden/.
#include "render_fixture.hpp"

#include <oxwald/core/scalability.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/render/runtime_renderer.hpp>
#include <oxwald/runtime/console.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/settings.hpp>
#include <oxwald/ui/ui.hpp>

#include <RmlUi/Core.h>
#include <imgui.h>

#include <filesystem>
#include <fstream>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;
using namespace ox::ui;

namespace {

constexpr u32 kW = 320, kH = 200;

class UiGpuTest : public RenderTest {
protected:
    Image renderUi() {
        CVarScope sky("r.Sky", "false"); // UI only: the scene stays black and independent of other features
        Options o;
        o.width = kW;
        o.height = kH;
        return render(camera({0, 1, 4}, {0, 0, 0}, 12.0f), o);
    }
};

} // namespace

TEST_F(UiGpuTest, ImGuiTextAndFilledRect) {
    UiConfig cfg;
    cfg.gameUI = false;
    UiSystem ui(cfg);
    UiOverlayFeature& feature = attachRenderer(*renderer, ui.bridgeShared());
    auto build = [&] {
        ui.beginFrame(1.0 / 60.0, {kW, kH}, 1.0f);
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(f32(kW), f32(kH)), IM_COL32(20, 30, 60, 255), IM_COL32(60, 20, 50, 255),
                                    IM_COL32(20, 60, 40, 255), IM_COL32(10, 10, 10, 255));
        bg->AddRectFilled(ImVec2(16, 16), ImVec2(136, 96), IM_COL32(230, 90, 40, 255), 8.0f);
        bg->AddRectFilled(ImVec2(80, 60), ImVec2(200, 150), IM_COL32(255, 255, 255, 128)); // 50 % white over both
        ImGui::SetNextWindowPos(ImVec2(150, 20));
        ImGui::SetNextWindowSize(ImVec2(160, 110));
        ImGui::Begin("ImGui", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("Hello, ImGui");
        ImGui::Text("Привет, мир");
        ImGui::Button("Button");
        ImGui::End();
        ui.endFrame();
    };
    build();
    build(); // auto-sized parts settle on the second frame
    Image img = renderUi();
    EXPECT_GT(feature.residentTextures(), 0u) << "font atlas uploaded";

    // Opaque rect is exactly its colour (display-encoded target, no conversion).
    const glm::u8vec4 rect = img.at(30, 30);
    EXPECT_NEAR(rect.r, 230, 2);
    EXPECT_NEAR(rect.g, 90, 2);
    EXPECT_NEAR(rect.b, 40, 2);
    // 50 % white over the rect: (230 + 255) / 2 in display space.
    const glm::u8vec4 blend = img.at(100, 80);
    EXPECT_NEAR(blend.r, (230 + 255) / 2, 3);
    EXPECT_NEAR(blend.g, (90 + 255) / 2, 3);
    GoldenResult g = compareGolden("ui_imgui", img);
    EXPECT_TRUE(g.matched) << g.message;
}

TEST_F(UiGpuTest, RmlUiStyledDocumentWithCyrillicText) {
    const std::filesystem::path dir = std::filesystem::path(OX_TEST_DATA_DIR) / "rml";
    UiConfig cfg;
    cfg.imgui = false;
    cfg.gameUIConfig.root = dir.string() + "/";
    cfg.gameUIConfig.hotReload = false;
    UiSystem ui(cfg);
    attachRenderer(*renderer, ui.bridgeShared());
    ASSERT_NE(ui.gameUI()->load("golden.rml", true), nullptr);
    for (int i = 0; i < 2; ++i) {
        ui.beginFrame(1.0 / 60.0, {kW, kH}, 1.0f);
        ui.endFrame();
    }
    EXPECT_EQ(ui.gameUI()->errorCount(), 0u);
    Image img = renderUi();
    const glm::u8vec4 page = img.at(4, 190);
    EXPECT_NEAR(page.r, 0x1b, 2);
    EXPECT_NEAR(page.g, 0x24, 2);
    EXPECT_NEAR(page.b, 0x33, 2);
    const glm::u8vec4 panel = img.at(24, 140);
    EXPECT_NEAR(panel.r, 0x2f, 2);
    EXPECT_NEAR(panel.g, 0x6f, 2);
    EXPECT_NEAR(panel.b, 0xe0, 2);
    GoldenResult g = compareGolden("ui_rmlui", img);
    EXPECT_TRUE(g.matched) << g.message;
}

TEST_F(UiGpuTest, DebugOverlayWindowsRender) {
    UiConfig cfg;
    cfg.gameUI = false;
    UiSystem ui(cfg);
    attachRenderer(*renderer, ui.bridgeShared());
    Console console;
    DebugTools& tools = ui.enableDebugTools({.console = &console, .world = world.get()});
    ui.imgui()->setVisible(true);
    tools.open(DebugTools::kStats);
    tools.open(DebugTools::kRenderGraph);
    mesh(Primitive::Cube, material({0.8f, 0.8f, 0.8f, 1}, 0.0f, 0.5f), {0, 0, 0});
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f);
    environment();
    console.print("Привет из консоли");
    (void)console.execute("r.Shadows.CSM.Cascades 3");
    for (int i = 0; i < 3; ++i) {
        ui.beginFrame(1.0 / 60.0, {960, 600}, 1.0f);
        ui.endFrame();
        Options o;
        o.width = 960;
        o.height = 600;
        o.frames = 1;
        render(camera({0, 1, 4}, {0, 0, 0}, 12.0f), o);
    }
    ASSERT_TRUE(ui.bridge().hasRenderInfo());
    const RenderInfo info = ui.bridge().renderInfo();
    EXPECT_GT(info.stats.drawCalls, 0u);
    EXPECT_EQ(info.outputSize, glm::uvec2(960, 600));

    const auto graph = ui.bridge().renderGraph();
    ASSERT_TRUE(graph.has_value());
    bool hasUi = false, hasTonemap = false;
    for (const RenderGraphPassInfo& p : graph->passes) {
        hasUi |= p.name == "UI";
        hasTonemap |= p.name == "Tonemap";
    }
    EXPECT_TRUE(hasUi);
    EXPECT_TRUE(hasTonemap);
    EXPECT_FALSE(graph->graphviz.empty());
    // Look at the output in <temp>/oxwald_render_out/ui_debug_overlay.png (not a golden: GPU timings change).
    ui.beginFrame(1.0 / 60.0, {960, 600}, 1.0f);
    ui.endFrame();
    Options o;
    o.width = 960;
    o.height = 600;
    writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / "ui_debug_overlay.png",
             render(camera({0, 1, 4}, {0, 0, 0}, 12.0f), o));
}

TEST_F(UiGpuTest, TexturesFollowTheStore) {
    UiConfig cfg;
    cfg.gameUI = false;
    UiSystem ui(cfg);
    UiOverlayFeature& feature = attachRenderer(*renderer, ui.bridgeShared());
    const u32 id = ui.bridge().textures().create(2, 2, std::vector<u8>(16, 255), "test");
    auto build = [&] {
        ui.beginFrame(1.0 / 60.0, {kW, kH}, 1.0f);
        ImGui::GetBackgroundDrawList()->AddImage(ImTextureRef(ImTextureID(id)), ImVec2(0, 0), ImVec2(32, 32));
        ui.endFrame();
    };
    build();
    Image img = renderUi();
    const usize withTexture = feature.residentTextures();
    EXPECT_GE(withTexture, 2u); // font atlas + ours
    EXPECT_EQ(img.at(10, 10), glm::u8vec4(255, 255, 255, 255));
    ui.bridge().textures().destroy(id);
    build();
    renderUi();
    EXPECT_EQ(feature.residentTextures(), withTexture - 1);
}

// Full runtime path: UiModule + withUi(render::createRenderer()) on a headless engine with the threaded render
// pipeline: UI frames reach the renderer, render info comes back, the input filter routes events, console commands
// and the "Auto" quality benchmark (run between frames on the render thread) work.
TEST(UiEngine, RuntimeIntegration) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("oxwald_ui_engine_" + Uuid::generate().toString());
    Engine engine;
    UiConfig cfg;
    cfg.imguiConfig.visible = false;
    engine.addModule(std::make_unique<UiModule>(cfg));
    engine.setRenderer(withUi(render::createRenderer({.headlessWidth = 640, .headlessHeight = 360})));
    EngineConfig ec;
    ec.appName = "OxwaldUiTests";
    ec.headless = true;
    ec.workerThreads = 2;
    ec.userDir = dir / "user";
    ec.loadUserSettings = false;
    ec.saveUserSettingsOnShutdown = false;
    if (Status st = engine.init(ec); !st) {
        std::filesystem::remove_all(dir);
        GTEST_SKIP() << "engine init failed (no Vulkan?): " << st.error().message;
    }
    auto* ui = engine.services().tryGet<UiSystem>();
    ASSERT_NE(ui, nullptr);
    for (int i = 0; i < 6; ++i) engine.tick(1.0 / 60.0);
    EXPECT_TRUE(ui->bridge().hasRenderInfo()) << "the UI feature ran inside the runtime renderer";
    ASSERT_TRUE(ui->lastFrame());

    // Input routing through the engine's InputSystem: F1 toggles the overlay and never reaches the game.
    engine.input().inject(InputEvent::key(Key::F1, true));
    engine.tick(1.0 / 60.0);
    EXPECT_TRUE(ui->imgui()->visible());
    EXPECT_FALSE(engine.input().keyPressed(Key::F1));
    engine.input().inject(InputEvent::key(Key::F1, false));
    ASSERT_TRUE(engine.console().execute("ui.debug").hasValue());
    EXPECT_FALSE(ui->imgui()->visible());

    // "Auto" quality from the settings menu model: benchmark on the render thread, applied on the game thread.
    std::string summary;
    ScopedConnection c = ui->qualityAutoDetected.connect([&](const std::string& s) { summary = s; });
    ui->bridge().autoDetectRequested.store(true);
    for (int i = 0; i < 10 && summary.empty(); ++i) engine.tick(1.0 / 60.0);
    EXPECT_TRUE(summary.starts_with("Auto: ")) << summary;
    EXPECT_NE(engine.settings().user().graphics.quality, "") << "settings captured from the applied levels";
    engine.shutdown();
    scalability::setOverall(QualityLevel::High);
    std::error_code ec2;
    std::filesystem::remove_all(dir, ec2);
}

// Cost of the UI pass at 1080p with a busy debug overlay (Stats, Console, Settings & CVars, Render Graph) on top of
// the sample settings menu (RmlUi). Prints the GPU time of the "UI" pass (see docs/dev/modules/ui.md).
TEST_F(UiGpuTest, PerfReport1080p) {
    UiConfig cfg;
    cfg.gameUIConfig.root = (resourceDir() / "sample").string() + "/";
    cfg.gameUIConfig.hotReload = false;
    UiSystem ui(cfg);
    attachRenderer(*renderer, ui.bridgeShared());
    Console console;
    Settings settings;
    ASSERT_TRUE(ui.gameUI()->bindSettingsMenu(settings));
    ASSERT_NE(ui.gameUI()->load("settings.rml", true), nullptr);
    DebugTools& tools = ui.enableDebugTools({.console = &console, .settings = &settings, .world = world.get()});
    ui.imgui()->setVisible(true);
    for (std::string_view w : {DebugTools::kStats, DebugTools::kConsole, DebugTools::kSettings, DebugTools::kRenderGraph}) tools.open(w);
    for (int i = 0; i < 200; ++i) console.print(std::format("log line {} — строка журнала", i));
    mesh(Primitive::Sphere, material({0.8f, 0.3f, 0.2f, 1}, 0.0f, 0.4f), {0, 0, 0});
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f);
    environment();
    for (int i = 0; i < 3; ++i) {
        ui.beginFrame(1.0 / 60.0, {1920, 1080}, 1.0f);
        ui.endFrame();
    }
    Options o;
    o.width = 1920;
    o.height = 1080;
    o.frames = 6;
    Image img = render(camera({0, 1, 4}, {0, 0, 0}, 12.0f), o);
    writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / "ui_perf_1080p.png", img);
    const auto f = ui.lastFrame();
    f64 uiMs = -1.0;
    for (const PassTiming& p : renderer->stats().passes)
        if (p.name == "UI" || p.name.ends_with("/UI")) uiMs = p.gpuMs;
    std::printf("UI 1080p: %zu draw commands, %zu vertices, %zu indices; UI pass GPU %.3f ms (frame %.3f ms)\n",
                f->commands.size(), f->vertices.size(), f->indices.size(), uiMs, renderer->stats().gpuFrameMs);
    EXPECT_GT(f->commands.size(), 50u);
}
