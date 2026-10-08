// CPU tests of the ui module (no Vulkan device): ImGui frames, RmlUi documents, data binding, hot reload, input
// routing, debug console, settings menu model, Lua bindings.
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/render/render_settings.hpp>
#include <oxwald/runtime/console.hpp>
#include <oxwald/runtime/input.hpp>
#include <oxwald/runtime/settings.hpp>
#include <oxwald/ui/ui.hpp>

#if OX_UI_HAS_SCRIPT
#include <oxwald/script/script_vm.hpp>
#endif

#include <RmlUi/Core.h>
#include <imgui.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <thread>

using namespace ox;
using namespace ox::ui;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        m_path = fs::temp_directory_path() / ("oxwald_ui_" + Uuid::generate().toString());
        fs::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    [[nodiscard]] const fs::path& path() const { return m_path; }
    void write(const std::string& name, const std::string& text) const {
        std::ofstream(m_path / name, std::ios::binary) << text;
    }

private:
    fs::path m_path;
};

UiConfig gameOnly(const TempDir& dir) {
    UiConfig c;
    c.imgui = false;
    c.gameUIConfig.root = dir.path().string() + "/";
    c.gameUIConfig.hotReload = false;
    return c;
}

constexpr const char* kStyle = R"(
body { font-family: Inter; font-size: 16px; color: #ffffff; width: 100%; height: 100%; }
div, p { display: block; }
#box { width: 200px; height: 100px; background-color: #ff0000; }
)";

void frame(UiSystem& ui, glm::uvec2 size = {640, 360}) {
    ui.beginFrame(1.0 / 60.0, size, 1.0f);
    ui.endFrame();
}

Rml::Colourb background(Rml::Element* e) { return e->GetComputedValues().background_color(); }

} // namespace

TEST(UiImGui, FrameProducesDrawDataAndFontTextures) {
    UiRenderBridge bridge;
    ImGuiLayer layer(bridge.textures());
    UiFrame out;
    for (int i = 0; i < 2; ++i) { // new auto-sized windows are hidden for their first frame
        out = {};
        layer.beginFrame(1.0 / 60.0, {800, 600}, 2.0f);
        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::Begin("Test");
        ImGui::Text("Hello, Оксвальд");
        ImGui::End();
        layer.endFrame(out);
    }
    EXPECT_EQ(out.size, glm::uvec2(800, 600));
    ASSERT_FALSE(out.commands.empty());
    EXPECT_FALSE(out.vertices.empty());
    EXPECT_GE(bridge.textures().size(), 1u) << "font atlas uploaded to the store";
    bool textured = false;
    for (const UiDrawCmd& c : out.commands) {
        EXPECT_LE(c.firstIndex + c.indexCount, out.indices.size());
        EXPECT_EQ(c.flags & kUiPremultiplied, 0u);
        EXPECT_FLOAT_EQ(c.xform.x, 2.0f) << "framebuffer scale applied";
        textured |= c.texture != 0 && bridge.textures().contains(u32(c.texture));
    }
    EXPECT_TRUE(textured);
}

TEST(UiImGui, BindlessTextureIdsPassThrough) {
    UiRenderBridge bridge;
    ImGuiLayer layer(bridge.textures());
    UiFrame out;
    for (int i = 0; i < 2; ++i) {
        out = {};
        layer.beginFrame(0.016, {400, 300});
        ImGui::Begin("Image");
        ImGui::Image(ImTextureRef(ImGuiLayer::textureId(42)), ImVec2(64, 64));
        ImGui::End();
        layer.endFrame(out);
    }
    bool found = false;
    for (const UiDrawCmd& c : out.commands) found |= c.texture == bindlessTexture(42);
    EXPECT_TRUE(found);
}

TEST(UiImGui, ToggleKeyShowsOverlay) {
    UiRenderBridge bridge;
    ImGuiLayer layer(bridge.textures());
    EXPECT_FALSE(layer.visible());
    EXPECT_TRUE(layer.processEvent(InputEvent::key(Key::F1, true)));
    EXPECT_TRUE(layer.visible());
    EXPECT_FALSE(layer.processEvent(InputEvent::key(Key::F1, false)));
    EXPECT_TRUE(layer.processEvent(InputEvent::key(Key::GraveAccent, true)));
    EXPECT_TRUE(layer.processEvent(InputEvent::text('`'))) << "the toggle character is not typed";
    EXPECT_FALSE(layer.visible());
}

TEST(UiGame, DataBindingUpdatesElementText) {
    TempDir dir;
    dir.write("style.rcss", kStyle);
    dir.write("doc.rml", R"(<rml><head><link type="text/rcss" href="style.rcss"/></head>
        <body data-model="stats"><p id="hp">HP {{health}}</p><p id="name">{{name}}</p></body></rml>)");
    UiSystem ui(gameOnly(dir));
    GameUI& g = *ui.gameUI();
    int health = 100;
    Rml::String name = "Игрок";
    {
        Rml::DataModelConstructor c = g.createModel("stats");
        ASSERT_TRUE(bool(c));
        c.Bind("health", &health);
        c.Bind("name", &name);
    }
    Rml::ElementDocument* doc = g.load("doc.rml", true);
    ASSERT_NE(doc, nullptr);
    frame(ui);
    Rml::Element* hp = doc->GetElementById("hp");
    ASSERT_NE(hp, nullptr);
    EXPECT_EQ(hp->GetInnerRML(), "HP 100");
    EXPECT_EQ(doc->GetElementById("name")->GetInnerRML(), "Игрок");

    health = 42;
    g.model("stats").DirtyVariable("health");
    frame(ui);
    EXPECT_EQ(hp->GetInnerRML(), "HP 42");
    EXPECT_EQ(g.errorCount(), 0u);
}

TEST(UiGame, HotReloadOfRcssChangesColour) {
    TempDir dir;
    dir.write("style.rcss", kStyle);
    dir.write("doc.rml", R"(<rml><head><link type="text/rcss" href="style.rcss"/></head><body><div id="box"/></body></rml>)");
    UiConfig cfg = gameOnly(dir);
    cfg.gameUIConfig.hotReload = true;
    cfg.gameUIConfig.hotReloadInterval = 0.0;
    UiSystem ui(cfg);
    GameUI& g = *ui.gameUI();
    ASSERT_NE(g.load("doc.rml", true), nullptr);
    frame(ui);
    EXPECT_EQ(background(g.document("doc.rml")->GetElementById("box")), Rml::Colourb(255, 0, 0, 255));

    std::string changed = kStyle;
    changed.replace(changed.find("#ff0000"), 7, "#00ff00");
    dir.write("style.rcss", changed);
    fs::last_write_time(dir.path() / "style.rcss", fs::last_write_time(dir.path() / "style.rcss") + std::chrono::seconds(2));
    int reloaded = 0;
    ScopedConnection c = g.documentReloaded.connect([&](const std::string&) { ++reloaded; });
    frame(ui); // update() polls (interval 0) and reloads
    EXPECT_EQ(reloaded, 1);
    Rml::ElementDocument* doc = g.document("doc.rml");
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(g.isVisible("doc.rml"));
    EXPECT_EQ(background(doc->GetElementById("box")), Rml::Colourb(0, 255, 0, 255));
}

TEST(UiGame, RendersIntoFrameWithPremultipliedCommands) {
    TempDir dir;
    dir.write("style.rcss", kStyle);
    dir.write("doc.rml", R"(<rml><head><link type="text/rcss" href="style.rcss"/></head><body><div id="box">Текст</div></body></rml>)");
    UiSystem ui(gameOnly(dir));
    ASSERT_NE(ui.gameUI()->load("doc.rml", true), nullptr);
    frame(ui);
    auto f = ui.lastFrame();
    ASSERT_TRUE(f);
    EXPECT_EQ(f->size, glm::uvec2(640, 360));
    ASSERT_GE(f->commands.size(), 2u) << "box background + text";
    for (const UiDrawCmd& c : f->commands) EXPECT_NE(c.flags & kUiPremultiplied, 0u);
    EXPECT_EQ(ui.bridge().latest(), f);
}

TEST(UiInput, UiConsumesClickGameDoesNot) {
    TempDir dir;
    dir.write("style.rcss", kStyle);
    dir.write("doc.rml", R"(<rml><head><link type="text/rcss" href="style.rcss"/>
        <style>body { pointer-events: none; } #box { pointer-events: auto; position: absolute; left: 0; top: 0; }</style></head>
        <body><div id="box"/></body></rml>)");
    UiSystem ui(gameOnly(dir));
    ASSERT_NE(ui.gameUI()->load("doc.rml", true), nullptr);
    int clicks = 0;
    ui.gameUI()->addEventListener("doc.rml", "box", "click", [&](Rml::Event&) { ++clicks; });
    InputSystem input;
    input.setEventFilter([&](const InputEvent& e) { return ui.processEvent(e); });
    frame(ui);

    // Click on the box: the UI consumes it.
    input.inject(InputEvent::mouseMove({50, 50}, {0, 0}));
    input.inject(InputEvent::mouseButton(MouseButton::Left, true));
    input.update(0.016);
    frame(ui);
    EXPECT_FALSE(input.mousePressed(MouseButton::Left));
    EXPECT_FALSE(input.mouseDown(MouseButton::Left));
    input.inject(InputEvent::mouseButton(MouseButton::Left, false));
    input.update(0.016);
    frame(ui);
    EXPECT_EQ(clicks, 1);
    EXPECT_EQ(input.mousePosition(), glm::vec2(50, 50)) << "mouse moves always reach the game";

    // Click outside (the body lets clicks through): the game gets it.
    input.inject(InputEvent::mouseMove({400, 300}, {0, 0}));
    input.inject(InputEvent::mouseButton(MouseButton::Left, true));
    input.update(0.016);
    EXPECT_TRUE(input.mousePressed(MouseButton::Left));
    input.inject(InputEvent::mouseButton(MouseButton::Left, false));
    input.update(0.016);
    EXPECT_FALSE(input.mouseDown(MouseButton::Left));
    EXPECT_EQ(clicks, 1);
}

TEST(UiInput, ImGuiWindowConsumesBeforeGameUiAndGame) {
    TempDir dir;
    UiConfig cfg = gameOnly(dir);
    cfg.imgui = true;
    UiSystem ui(cfg);
    InputSystem input;
    input.setEventFilter([&](const InputEvent& e) { return ui.processEvent(e); });
    auto window = [&] {
        ui.beginFrame(0.016, {640, 360});
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(200, 200));
        ImGui::Begin("Blocker");
        ImGui::Text("debug");
        ImGui::End();
        ui.endFrame();
    };
    input.inject(InputEvent::mouseMove({100, 100}, {0, 0}));
    input.update(0.016);
    window();
    window(); // hover is evaluated in NewFrame
    EXPECT_TRUE(ui.imgui()->wantsMouse());
    input.inject(InputEvent::mouseButton(MouseButton::Left, true));
    input.update(0.016);
    EXPECT_FALSE(input.mousePressed(MouseButton::Left));
    input.inject(InputEvent::mouseButton(MouseButton::Left, false));
    input.inject(InputEvent::mouseMove({500, 300}, {0, 0}));
    input.update(0.016);
    for (int i = 0; i < 3; ++i) window(); // ImGui trickles queued input: down, up, then the move
    EXPECT_FALSE(ui.imgui()->wantsMouse());
    input.inject(InputEvent::mouseButton(MouseButton::Left, true));
    input.update(0.016);
    EXPECT_TRUE(input.mousePressed(MouseButton::Left));
}

static CVar<int> cvConsoleTarget("ui.Test.ConsoleTarget", 1, "test cvar set through the debug console window");

TEST(UiDebug, ConsoleWindowExecutesCVarCommand) {
    TempDir dir;
    UiConfig cfg = gameOnly(dir);
    cfg.imgui = true;
    cfg.gameUI = false;
    UiSystem ui(cfg);
    Console console;
    DebugTools& tools = ui.enableDebugTools({.console = &console});
    ui.imgui()->setVisible(true);
    tools.open(DebugTools::kConsole);
    InputSystem input;
    input.setEventFilter([&](const InputEvent& e) { return ui.processEvent(e); });
    for (int i = 0; i < 3; ++i) frame(ui); // window appears, input field takes keyboard focus

    for (char ch : std::string("ui.Test.ConsoleTarget 42")) input.inject(InputEvent::text(u32(ch)));
    input.update(0.016);
    frame(ui);
    input.inject(InputEvent::key(Key::Enter, true));
    input.update(0.016);
    EXPECT_FALSE(input.keyPressed(Key::Enter)) << "typing into the console must not reach the game";
    frame(ui);
    input.inject(InputEvent::key(Key::Enter, false));
    input.update(0.016);
    frame(ui);

    EXPECT_EQ(cvConsoleTarget.get(), 42);
    ASSERT_FALSE(console.history().empty());
    EXPECT_EQ(console.history().back(), "ui.Test.ConsoleTarget 42");

    tools.consoleSubmit("ui.Test.ConsoleTarget 7");
    EXPECT_EQ(cvConsoleTarget.get(), 7);
    cvConsoleTarget.reset();
}

TEST(UiDebug, AllToolWindowsDrawWithoutEngine) {
    TempDir dir;
    UiConfig cfg = gameOnly(dir);
    cfg.imgui = true;
    UiSystem ui(cfg);
    Console console;
    Settings settings;
    DebugTools& tools = ui.enableDebugTools({.console = &console, .settings = &settings});
    ui.imgui()->setVisible(true);
    for (std::string_view w : {DebugTools::kStats, DebugTools::kConsole, DebugTools::kSettings, DebugTools::kInspector,
                               DebugTools::kRenderGraph, DebugTools::kCoroutines, DebugTools::kDebugDraw})
        tools.open(w);
    CVarRegistry::instance().set("ui.ShowStats", "true", CVarSource::Code);
    for (int i = 0; i < 3; ++i) frame(ui);
    EXPECT_TRUE(ui.bridge().captureRenderGraph.load()) << "render graph viewer requests captures while open";
    EXPECT_GT(ui.lastFrame()->commands.size(), 10u);
    EXPECT_GT(ui.lastFrame()->vertices.size(), 2000u);
    CVarRegistry::instance().set("ui.ShowStats", "false", CVarSource::Code);
}

TEST(UiSettings, SettingsMenuChangesQualityCVars) {
    render::registerRenderCVars();
    UiConfig cfg;
    cfg.imgui = false;
    cfg.gameUIConfig.hotReload = false;
    cfg.gameUIConfig.root = (resourceDir() / "sample").string() + "/";
    UiSystem ui(cfg);
    GameUI& g = *ui.gameUI();
    Settings settings;
    scalability::setOverall(QualityLevel::High);
    settings.captureFromCVars();
    bool autoRequested = false;
    SettingsMenuHooks hooks;
    hooks.requestAutoDetect = [&] { autoRequested = true; };
    hooks.rayTracingAvailable = [](std::string& reason) {
        reason = "MoltenVK does not implement Vulkan ray tracing";
        return false;
    };
    hooks.upscalers = [] {
        return std::vector<UpscalerAvailability>{{"Off", true, {}}, {"FSR1", true, {}}, {"DLSS", false, "needs an NVIDIA RTX GPU"}};
    };
    ASSERT_TRUE(g.bindSettingsMenu(settings, hooks));
    Rml::ElementDocument* doc = g.load("settings.rml", true);
    ASSERT_NE(doc, nullptr);
    frame(ui, {1280, 720});
    frame(ui, {1280, 720});
    EXPECT_EQ(g.errorCount(), 0u);

    Rml::Element* low = doc->GetElementById("quality-Low");
    ASSERT_NE(low, nullptr);
    EXPECT_FALSE(low->IsClassSet("selected"));
    EXPECT_TRUE(doc->GetElementById("quality-High")->IsClassSet("selected"));
    low->Click();
    frame(ui, {1280, 720});
    EXPECT_EQ(scalability::overallLevel(), QualityLevel::Low);
    EXPECT_EQ(scalability::currentLevel(Scalability::Shadows), QualityLevel::Low);
    EXPECT_EQ(CVarRegistry::instance().find("r.Shadows.CSM.Resolution")->toString(), "1024");
    EXPECT_EQ(settings.user().graphics.quality, "Low");
    EXPECT_TRUE(low->IsClassSet("selected"));

    doc->GetElementById("quality-Ultra")->Click();
    frame(ui, {1280, 720});
    EXPECT_EQ(scalability::overallLevel(), QualityLevel::Ultra);
    EXPECT_EQ(CVarRegistry::instance().find("r.Shadows.CSM.Resolution")->toString(), "4096");

    doc->GetElementById("quality-Auto")->Click();
    frame(ui, {1280, 720});
    EXPECT_TRUE(autoRequested);
    EXPECT_TRUE(doc->GetElementById("quality-Auto")->IsClassSet("selected"));

    // Unavailable options stay unavailable (greyed + reason) and cannot be selected.
    Rml::Element* dlss = doc->GetElementById("upscaler-DLSS");
    ASSERT_NE(dlss, nullptr);
    EXPECT_TRUE(dlss->IsClassSet("unavailable"));
    EXPECT_EQ(dlss->GetAttribute<Rml::String>("title", ""), "needs an NVIDIA RTX GPU");
    dlss->Click();
    frame(ui, {1280, 720});
    EXPECT_NE(settings.user().graphics.upscaler, "DLSS");
    doc->GetElementById("upscaler-FSR1")->Click();
    frame(ui, {1280, 720});
    EXPECT_EQ(settings.user().graphics.upscaler, "FSR1");
    EXPECT_TRUE(doc->GetElementById("raytracing")->HasAttribute("disabled"));

    // Two-way binding: the checkbox toggles vsync in the user settings.
    const bool vsync = settings.user().graphics.vsync;
    doc->GetElementById("vsync")->Click();
    frame(ui, {1280, 720});
    EXPECT_EQ(settings.user().graphics.vsync, !vsync);
    EXPECT_EQ(g.errorCount(), 0u);
    scalability::setOverall(QualityLevel::High);
}

TEST(UiSamples, SampleDocumentsLoadWithoutErrors) {
    UiConfig cfg;
    cfg.imgui = false;
    cfg.gameUIConfig.hotReload = false;
    cfg.gameUIConfig.root = (resourceDir() / "sample").string() + "/";
    UiSystem ui(cfg);
    GameUI& g = *ui.gameUI();
    Settings settings;
    ASSERT_TRUE(g.bindSettingsMenu(settings));
    int health = 80, maxHealth = 100, ammo = 12, reserve = 60;
    Rml::String objective = "Найдите выход";
    {
        Rml::DataModelConstructor c = g.createModel("hud");
        c.Bind("health", &health);
        c.Bind("maxHealth", &maxHealth);
        c.Bind("ammo", &ammo);
        c.Bind("reserve", &reserve);
        c.Bind("objective", &objective);
    }
    for (const char* doc : {"main_menu.rml", "hud.rml", "settings.rml"}) {
        EXPECT_NE(g.load(doc, true), nullptr) << doc;
        frame(ui, {1600, 900});
    }
    EXPECT_EQ(g.errorCount(), 0u);
    EXPECT_GT(ui.lastFrame()->commands.size(), 20u);
}

#if OX_UI_HAS_SCRIPT
TEST(UiLua, DebugWindowAndUiModelFromScripts) {
    TempDir dir;
    dir.write("style.rcss", kStyle);
    dir.write("hud.rml", R"RML(<rml><head><link type="text/rcss" href="style.rcss"/></head>
        <body data-model="hud"><p id="hp">{{health}}</p><div id="box" data-event-click="hit(5)"/></body></rml>)RML");
    UiConfig cfg = gameOnly(dir);
    cfg.imgui = true;
    script::ScriptVM vm; // must outlive the UiSystem's Lua bindings (or call ui.unbindLua())
    UiSystem ui(cfg);
    ui.bindLua(vm);
    sol::environment env = vm.createEnvironment();
    ASSERT_TRUE(vm.runString(R"(
        hits = 0
        model = ui.createModel("hud", { health = 100 }, { hit = function(args) hits = hits + args[1] end })
        loaded = ui.load("hud.rml", true)
    )", "setup", &env));
    EXPECT_TRUE(env["loaded"].get<bool>());

    ui.beginFrame(0.016, {640, 360});
    ASSERT_TRUE(vm.runString(R"(
        model:set("health", 55)
        shown = debug.window("Lua debug", function()
            debug.text("hp", model:get("health"))
            debug.button("press")
            local v, changed = debug.checkbox("flag", true)
            local f = debug.sliderFloat("speed", 0.5, 0, 1)
            local s = debug.inputText("name", "Оксвальд")
            local c = debug.colorEdit("tint", vec4(1, 0.5, 0.25, 1))
            debug.plotLines("graph", {1, 3, 2, 5})
        end)
        debug.beginWindow("Dangling")  -- never closed: closed automatically at frame end
    )", "frame", &env));
    ui.endFrame();
    EXPECT_TRUE(env["shown"].get<bool>());
    frame(ui);
    Rml::ElementDocument* doc = ui.gameUI()->document("hud.rml");
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->GetElementById("hp")->GetInnerRML(), "55");
    doc->GetElementById("box")->Click();
    frame(ui);
    EXPECT_EQ(vm.errorCount(), 0u) << vm.lastError();
    EXPECT_EQ(env["hits"].get_or(-1.0), 5.0);
    EXPECT_EQ(vm.errorCount(), 0u);
}
#endif
