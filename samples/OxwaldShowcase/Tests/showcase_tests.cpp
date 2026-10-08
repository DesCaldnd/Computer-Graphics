// OxwaldShowcase: Lua scripts compile; the save station survives a save/load round trip in a headless engine.
#include "showcase.hpp"

#include <oxwald/render/runtime_renderer.hpp>
#include <oxwald/runtime/runtime.hpp>
#include <oxwald/script/script_vm.hpp>
#include <oxwald/ui/ui_module.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ox;

namespace {

const fs::path kProject = OX_SHOWCASE_DIR;

std::vector<fs::path> luaFiles() {
    std::vector<fs::path> out;
    for (const auto& e : fs::recursive_directory_iterator(kProject / "Assets" / "Scripts"))
        if (e.is_regular_file() && e.path().extension() == ".lua") out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

// A headless engine on the showcase project (real renderer offscreen, UI module, Showcase game module).
struct ShowcaseEngine {
    Engine engine;
    explicit ShowcaseEngine(const std::string& scene, const fs::path& userDir) {
        engine.addModule(std::make_unique<ui::UiModule>());
        engine.setRenderer(ui::withUi(render::createRenderer()));
        EngineConfig cfg;
        cfg.appName = "ShowcaseTests";
        cfg.projectPath = kProject;
        cfg.headless = true;
        cfg.startupScene = scene;
        cfg.userDir = userDir;
        cfg.loadUserSettings = false;
        cfg.saveUserSettingsOnShutdown = false;
        cfg.threadedRendering = false;
        cfg.gameArgs = {"--photo"};
        status = engine.init(cfg);
    }
    void run(int frames) {
        for (int i = 0; i < frames; ++i) engine.tick(1.0 / 60.0);
    }
    u64 scriptErrors() {
        auto* vm = engine.services().tryGet<script::ScriptVM>();
        return vm ? vm->errorCount() : 0;
    }
    Status status;
};

} // namespace

TEST(ShowcaseScripts, AllCompile) {
    script::ScriptVM vm;
    vm.addSearchRoot(kProject / "Assets" / "Scripts");
    const auto files = luaFiles();
    ASSERT_GT(files.size(), 20u);
    for (const fs::path& f : files) {
        const auto asset = vm.loadScript(f);
        EXPECT_TRUE(asset != nullptr) << f;
    }
    EXPECT_EQ(vm.errorCount(), 0u) << vm.lastError();
}

TEST(ShowcaseSaves, RoundTripInSaveStation) {
    const fs::path user = fs::temp_directory_path() / "oxshowcase_tests_saves";
    fs::remove_all(user);
    ShowcaseEngine s("project://Assets/Scenes/Stations/14_SaveGames.oxscene", user);
    ASSERT_TRUE(s.status) << s.status.error().message;
    ASSERT_NE(showcase::activeModule(), nullptr) << "the project enables the Showcase game module";
    s.run(90); // crates settle

    World& w = s.engine.world();
    std::vector<Entity> crates;
    Entity lamp;
    for (auto [e, n] : w.view<NameComponent>().each()) {
        if (n.name == "SavedCrate") crates.push_back(w.wrap(e));
        if (n.name == "SavedLamp" && !lamp) lamp = w.wrap(e);
    }
    ASSERT_EQ(crates.size(), 6u);
    ASSERT_TRUE(lamp);
    const glm::vec3 savedPos = crates[0].worldPosition();
    const f32 savedIntensity = lamp.get<LightComponent>().intensity;
    const Uuid crateId = crates[0].uuid(), lampId = lamp.uuid(); // loading replaces the world (handles die)

    auto saved = s.engine.saveGame("roundtrip", "Round trip");
    ASSERT_TRUE(saved) << saved.error().message;

    // Change the world: push a crate away, switch the lamp.
    crates[0].setWorldPosition(savedPos + glm::vec3(5.0f, 2.0f, 0.0f));
    lamp.get<LightComponent>().intensity = savedIntensity > 0 ? 0.0f : 1234.0f;
    s.run(30);
    ASSERT_GT(glm::distance(crates[0].worldPosition(), savedPos), 1.0f);

    auto loaded = s.engine.loadGame("roundtrip");
    ASSERT_TRUE(loaded) << loaded.error().message;
    s.run(2);
    World& w2 = s.engine.world();
    Entity crate = w2.find(crateId);
    Entity lamp2 = w2.find(lampId);
    ASSERT_TRUE(crate);
    ASSERT_TRUE(lamp2);
    EXPECT_LT(glm::distance(crate.worldPosition(), savedPos), 0.05f);
    EXPECT_FLOAT_EQ(lamp2.get<LightComponent>().intensity, savedIntensity);
    bool listed = false;
    for (const SaveSlotInfo& slot : s.engine.saves().listSlots()) listed |= slot.header.slot == "roundtrip";
    EXPECT_TRUE(listed);
    EXPECT_EQ(s.scriptErrors(), 0u);
}
