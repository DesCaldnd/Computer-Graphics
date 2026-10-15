// OxwaldShowcase: Lua scripts compile; the save station survives a save/load round trip in a headless engine; the
// destruction station demo breaks, collapses and stays within the debris budget.
#include "showcase.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/gameplay/destruction.hpp>
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

// Destruction station: the scripted demo (photo mode) breaks a column -> the beam crumbles above it (Static
// anchors), a blast at the tower's foot collapses it, a shot holes the wall; chunks are rendered and the moving
// debris stays within the budget.
TEST(ShowcaseDestruction, StationDemoBreaksAndCollapses) {
    const fs::path user = fs::temp_directory_path() / "oxshowcase_tests_destruction";
    fs::remove_all(user);
    ShowcaseEngine s("project://Assets/Scenes/Stations/15_Destruction.oxscene", user);
    ASSERT_TRUE(s.status) << s.status.error().message;
    auto* rt = s.engine.services().tryGet<gameplay::DestructionRuntime>();
    ASSERT_NE(rt, nullptr);
    u32 fractured = 0, collapsed = 0;
    ScopedConnection c = rt->onDestruction.connect([&](const gameplay::DestructionEvent& ev) {
        if (ev.type == gameplay::DestructionEvent::Type::Fractured) ++fractured;
        if (ev.type == gameplay::DestructionEvent::Type::Collapsed) ++collapsed;
    });
    s.run(300); // demo: 0.6 s column, 1.3 s tower blast, 2.2 s wall, 3.0 s crates

    World& w = s.engine.world();
    const Entity column = w.findByName("Column.L");
    const Entity beam = w.findByName("Beam");
    const Entity tower = w.findByName("Tower");
    const Entity wall = w.findByName("Wall");
    ASSERT_TRUE(column && beam && tower && wall);
    EXPECT_TRUE(rt->isBroken(column));
    EXPECT_TRUE(rt->isBroken(beam)) << "the beam lost the column under it";
    EXPECT_TRUE(rt->isBroken(tower));
    EXPECT_TRUE(rt->isBroken(wall));
    EXPECT_GE(fractured, 4u);
    EXPECT_GE(collapsed, 1u);
    EXPECT_GT(rt->frozenChunkCount(beam), 0u) << "the end over the right column stays";
    EXPECT_GT(rt->frozenChunkCount(wall), 0u);
    EXPECT_LT(rt->frozenChunkCount(tower), 10u) << "the shaft above the blast collapsed";
    u32 chunksWithMesh = 0;
    for (auto [e, chunk, mr] : w.view<gameplay::DestructionChunkComponent, MeshRendererComponent>().each()) {
        (void)e;
        (void)chunk;
        chunksWithMesh += mr.mesh.isValid() ? 1u : 0u;
    }
    EXPECT_GT(chunksWithMesh, 50u);
    EXPECT_LE(rt->activeDebrisCount(), u32(std::stoi(CVarRegistry::instance().find("destruction.MaxActiveDebris")->toString())));
    EXPECT_EQ(s.scriptErrors(), 0u);
}
