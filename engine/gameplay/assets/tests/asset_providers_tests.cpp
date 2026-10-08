// Gameplay providers on top of the asset database (AssetRegistry + AssetManager) and hot reload into running
// gameplay instances (scripts, behaviour trees, prefabs).
#include "gameplay_test_utils.hpp"

#include <oxwald/assets/assets.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/gameplay/asset_providers.hpp>
#include <oxwald/scene/prefab.hpp>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;
namespace fs = std::filesystem;

namespace {

class TempProject {
public:
    TempProject() {
        static std::atomic<int> counter{0};
        m_path = fs::temp_directory_path() /
                 ("oxgameplay_assets_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::remove_all(m_path);
        fs::create_directories(m_path / "Assets");
    }
    ~TempProject() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    [[nodiscard]] const fs::path& path() const { return m_path; }
    [[nodiscard]] fs::path asset(const std::string& rel) const { return m_path / "Assets" / rel; }

    void write(const std::string& rel, std::string_view text) const {
        write(rel, std::span(reinterpret_cast<const std::byte*>(text.data()), text.size()));
    }
    void write(const std::string& rel, std::span<const std::byte> bytes) const {
        const fs::path p = asset(rel);
        const bool existed = fs::exists(p);
        const auto before = existed ? fs::last_write_time(p) : fs::file_time_type{};
        fs::create_directories(p.parent_path());
        {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        }
        // Make edits visible to the polling watcher even within the file system's mtime resolution.
        if (existed) fs::last_write_time(p, std::max(fs::last_write_time(p), before) + std::chrono::seconds(2));
    }

private:
    fs::path m_path;
};

std::vector<std::byte> wavMono16(u32 sampleRate, u32 frames) {
    std::vector<std::byte> out;
    auto put = [&](const void* p, usize n) {
        const auto* b = static_cast<const std::byte*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto u32le = [&](u32 v) { put(&v, 4); };
    auto u16le = [&](u16 v) { put(&v, 2); };
    put("RIFF", 4);
    u32le(36 + frames * 2);
    put("WAVEfmt ", 8);
    u32le(16);
    u16le(1); // PCM
    u16le(1); // mono
    u32le(sampleRate);
    u32le(sampleRate * 2);
    u16le(2);
    u16le(16);
    put("data", 4);
    u32le(frames * 2);
    for (u32 i = 0; i < frames; ++i) {
        const i16 s = i16(std::sin(f32(i) * 0.1f) * 8000.f);
        put(&s, 2);
    }
    return out;
}

constexpr std::string_view kCubeObj = R"(o Box
v -0.5 -0.5 -0.5
v 0.5 -0.5 -0.5
v 0.5 0.5 -0.5
v -0.5 0.5 -0.5
v -0.5 -0.5 0.5
v 0.5 -0.5 0.5
v 0.5 0.5 0.5
v -0.5 0.5 0.5
f 1 4 3
f 1 3 2
f 5 6 7
f 5 7 8
f 1 2 6
f 1 6 5
f 4 8 7
f 4 7 3
f 1 5 8
f 1 8 4
f 2 3 7
f 2 7 6
)";

// Version 1 of the crate prefab; later versions edit this document (the same file edited in the editor keeps its
// prefab-local entity ids — a freshly created prefab would describe new entities).
serial::Document crateDocument(const Uuid& prefabId) {
    World proto;
    Entity root = proto.create("Crate");
    root.add<LightComponent>().intensity = 100.f;
    return createPrefab(proto, root, {.prefabId = prefabId, .linkSource = false});
}

std::string crateBytes(serial::Document doc, f32 intensity) {
    serial::Value& entity = doc.root.find("entities")->items().front();
    entity.find("components")->find("Light")->set("intensity", serial::Value::makeF32(intensity));
    const auto bytes = serial::encodeBinary(doc);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string scriptSource(int version) {
    return "function onCreate(self) self.created = (self.created or 0) + 1 end\n"
           "function onUpdate(self, dt) self.entity.name = 'v" + std::to_string(version) + "' .. self.created end\n";
}

f64 bbNumber(const ai::Blackboard* bb, const std::string& key) {
    const ai::BlackboardValue* v = bb ? bb->find(key) : nullptr;
    if (!v) return -1.0;
    return std::visit(
        [](const auto& x) -> f64 {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_arithmetic_v<T>) return f64(x);
            return -2.0;
        },
        *v);
}

std::string treeSource(int value) {
    return R"({"root": {"type": "SetBlackboard", "key": "ver", "value": )" + std::to_string(value) + "}}";
}

// Registry + manager + providers over a temporary project.
struct AssetFixture {
    TempProject project;
    std::unique_ptr<assets::AssetRegistry> registry;
    std::unique_ptr<assets::AssetManager> manager;
    std::unique_ptr<AssetProviders> providers;

    void open() {
        assets::AssetRegistry::Options o;
        o.watchDebounce = std::chrono::milliseconds(10);
        registry = std::make_unique<assets::AssetRegistry>(project.path(), o);
        registerGameplayImporters(registry->importers());
        registry->scan();
        manager = std::make_unique<assets::AssetManager>(*registry);
        providers = std::make_unique<AssetProviders>(*manager);
    }
    Uuid id(std::string_view path) const { return registry->uuidForPath(path).value_or(Uuid{}); }
};

template <class Fn>
bool pollUntil(AssetFixture& f, Fn&& pred, const std::function<void()>& tick = {}) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        f.registry->poll();
        f.manager->update();
        if (tick) tick();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return pred();
}

} // namespace

TEST(GameplayAssets, ProvidersResolveEveryKindFromTheAssetDatabase) {
    registerGameplayTypes();
    AssetFixture f;
    f.project.write("Scripts/mover.lua", scriptSource(1));
    f.project.write("AI/patrol.oxbt", treeSource(1));
    const Uuid crateId = Uuid::fromName("test.crate");
    f.project.write("Prefabs/crate.oxprefab", crateBytes(crateDocument(crateId), 100.f));
    f.project.write("Models/box.obj", kCubeObj);
    f.project.write("Anim/locomotion.oxanimctrl", R"({
        "parameters": [{"name": "speed", "type": "Float", "defaultValue": 0}],
        "states": [{"name": "Idle"}, {"name": "Run", "speed": 1.5}],
        "transitions": [{"from": "Idle", "to": "Run", "parameter": "speed", "op": "Greater", "threshold": 0.1}],
        "defaultState": "Idle"})");
    std::vector<u16> heights{0, 65535, 32768, 0, 1000, 2000, 3000, 4000, 0, 0, 0, 0, 65535, 65535, 65535, 65535};
    f.project.write("Terrain/hills.r16", std::as_bytes(std::span(heights)));
    f.project.write("Audio/beep.wav", wavMono16(8000, 800));
    f.open();
    AssetProviders& p = *f.providers;

    // Scripts by path, path without extension, file stem and id.
    auto byPath = p.scriptByName("Scripts/mover.lua");
    ASSERT_TRUE(byPath);
    EXPECT_EQ(byPath->source, scriptSource(1));
    EXPECT_EQ(byPath->name, "Scripts/mover.lua");
    EXPECT_TRUE(p.scriptByName("Scripts/mover"));
    EXPECT_TRUE(p.scriptByName("mover"));
    EXPECT_TRUE(p.scriptById(f.id("Scripts/mover.lua")));
    EXPECT_FALSE(p.scriptByName("missing"));

    // Behaviour tree (Raw blob with kind BehaviorTree).
    auto tree = p.behaviorTree(f.id("AI/patrol.oxbt"));
    ASSERT_TRUE(tree);
    EXPECT_EQ((*tree)["root"]["type"], "SetBlackboard");
    EXPECT_FALSE(p.behaviorTree(f.id("Scripts/mover.lua"))) << "wrong type is rejected";

    // Prefabs by name; network type names.
    auto prefab = p.prefab("crate");
    ASSERT_TRUE(prefab);
    EXPECT_EQ(ox::prefabId(*prefab), crateId);
    EXPECT_TRUE(p.prefab("Prefabs/crate.oxprefab"));
    EXPECT_TRUE(p.prefab(f.id("Prefabs/crate.oxprefab").toString()));
    const auto names = p.prefabNames();
    EXPECT_NE(std::find(names.begin(), names.end(), "Prefabs/crate"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "crate"), names.end());

    // Mesh triangles of the model's mesh sub-asset.
    auto model = f.registry->info(f.id("Models/box.obj"));
    ASSERT_TRUE(model);
    Uuid meshId;
    for (const Uuid& sub : model->subAssets) {
        if (auto info = f.registry->info(sub); info && info->type == assets::AssetType::Mesh) meshId = sub;
    }
    ASSERT_TRUE(meshId.isValid());
    auto tris = p.meshTriangles(meshId);
    ASSERT_TRUE(tris);
    EXPECT_GE(tris->vertices.size(), 8u);
    EXPECT_EQ(tris->indices.size() % 3, 0u);
    EXPECT_GE(tris->indices.size(), 36u);

    // Animator controller asset -> anim::AnimatorController.
    auto ctrl = p.controller(f.id("Anim/locomotion.oxanimctrl"));
    ASSERT_TRUE(ctrl);
    ASSERT_EQ(ctrl->layers().size(), 1u);
    EXPECT_EQ(ctrl->layers()[0].states.size(), 2u);
    EXPECT_GE(ctrl->findParameter("speed"), 0);
    EXPECT_EQ(ctrl->layers()[0].transitions.size(), 1u);

    // Heightmap (r16) -> normalized samples.
    auto hm = p.heightmap(f.id("Terrain/hills.r16"));
    ASSERT_TRUE(hm);
    EXPECT_EQ(hm->resolution, 4u);
    EXPECT_FLOAT_EQ(hm->normalized[1], 1.f);
    EXPECT_NEAR(hm->normalized[2], 0.5f, 1e-4f);

    // Audio clip decoded into the (offline) engine.
    audio::AudioEngine engine;
    audio::AudioEngineConfig ac;
    ac.offline = true;
    ASSERT_TRUE(engine.init(ac));
    const audio::SoundId sound = p.sound(engine, f.id("Audio/beep.wav"));
    ASSERT_TRUE(sound.valid());
    EXPECT_NEAR(engine.soundDuration(sound), 0.1f, 0.01f);
    EXPECT_EQ(p.sound(engine, f.id("Audio/beep.wav")), sound) << "cached per engine";
    engine.shutdown();
}

TEST(GameplayAssets, HotReloadPropagatesToRunningScriptsTreesAndPrefabs) {
    registerGameplayTypes(); // before building prefab files: unregistered components are not serialized
    AssetFixture f;
    f.project.write("Scripts/mover.lua", scriptSource(1));
    f.project.write("AI/tree.oxbt", treeSource(1));
    const serial::Document crate1 = crateDocument(Uuid::fromName("test.crate.reload"));
    f.project.write("Prefabs/crate.oxprefab", crateBytes(crate1, 100.f));
    f.open();
    f.registry->startWatching();
    f.registry->poll(); // baseline

    GameplayHarness h(testConfig(), [&](Services& s) { f.providers->registerIn(s); });
    ASSERT_EQ(&h.services.get<IScriptSourceProvider>(), static_cast<IScriptSourceProvider*>(f.providers.get()));
    ASSERT_TRUE(h.services.has<GameplayAssetEvents>());

    Entity scripted = h.world.create("Scripted");
    scripted.add<ScriptComponent>().script = "mover";
    Entity agent = h.world.create("Agent");
    auto& bt = agent.add<BehaviorTreeComponent>();
    bt.tree = f.id("AI/tree.oxbt");
    h.start();

    auto& scripts = h.runtime<ScriptRuntime>();
    Entity crate = scripts.spawnPrefab("crate");
    ASSERT_TRUE(crate.valid());
    ASSERT_TRUE(crate.has<LightComponent>());
    EXPECT_FLOAT_EQ(crate.get<LightComponent>().intensity, 100.f);
    Entity crateOverride = scripts.spawnPrefab("Prefabs/crate.oxprefab");
    ASSERT_TRUE(crateOverride.valid());
    crateOverride.get<LightComponent>().intensity = 7.f;
    recordOverride(crateOverride, "Light.intensity");

    h.run(0.2);
    EXPECT_EQ(scripted.name(), "v11");
    auto& ai = h.runtime<AIRuntime>();
    ASSERT_NE(ai.blackboard(agent), nullptr);
    EXPECT_EQ(bbNumber(ai.blackboard(agent), "ver"), 1.0);
    ai.blackboard(agent)->set("kept", true);
    script::ScriptInstance* before = scripts.instance(scripted);

    std::vector<GameplayAssetChange> changes;
    ScopedConnection changed = h.services.get<GameplayAssetEvents>().changed.connect(
        [&](const GameplayAssetChange& c) { changes.push_back(c); });
    // Edit all three sources while the game runs.
    f.project.write("Scripts/mover.lua", scriptSource(2));
    f.project.write("AI/tree.oxbt", treeSource(2));
    f.project.write("Prefabs/crate.oxprefab", crateBytes(crate1, 500.f));

    EXPECT_TRUE(pollUntil(
        f,
        [&] {
            return scripted.name() == "v21" && ai.blackboard(agent) && bbNumber(ai.blackboard(agent), "ver") == 2.0 &&
                   crate.valid() && crate.tryGet<LightComponent>() && crate.get<LightComponent>().intensity == 500.f;
        },
        [&] { h.tick(); }))
        << "script '" << scripted.name() << "', tree ver " << bbNumber(ai.blackboard(agent), "ver") << ", light "
        << (crate.tryGet<LightComponent>() ? crate.get<LightComponent>().intensity : -1.f) << ", " << changes.size()
        << " change events";
    // Reloaded in place: same instance, `self` survived (onCreate not run again), blackboard values kept,
    // prefab overrides kept.
    EXPECT_EQ(scripts.instance(scripted), before);
    EXPECT_TRUE(ai.blackboard(agent)->getOr<bool>("kept", false));
    EXPECT_FLOAT_EQ(crateOverride.get<LightComponent>().intensity, 7.f);
    EXPECT_EQ(h.runtime<script::ScriptVM>().errorCount(), 0u);
}


